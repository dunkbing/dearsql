#include "ui/create_database_dialog.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/query_executor.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <cfloat>
#include <cstring>
#include <spdlog/spdlog.h>

namespace {
    constexpr const char* kPopupId = "Create Database###create_database_dialog";
    constexpr float kDialogWidth = 500.0f;
    constexpr float kLabelColumnW = 100.0f;
    constexpr float kButtonW = 90.0f;

    constexpr const char* kEncodings[] = {"UTF8",      "LATIN1",  "LATIN2",    "LATIN9", "WIN1252",
                                          "SQL_ASCII", "EUC_JP",  "EUC_KR",    "EUC_CN", "SJIS",
                                          "BIG5",      "WIN1251", "ISO_8859_5"};
    constexpr int kEncodingCount = sizeof(kEncodings) / sizeof(kEncodings[0]);

    constexpr const char* kCharsets[] = {"utf8mb4", "utf8mb3", "utf8",  "latin1", "ascii",
                                         "binary",  "utf16",   "utf32", "cp1251", "gbk",
                                         "big5",    "euckr",   "sjis"};
    constexpr int kCharsetCount = sizeof(kCharsets) / sizeof(kCharsets[0]);

    void fieldLabel(const char* text) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, "%s", text);
        ImGui::SameLine(kLabelColumnW);
    }

    void stringCombo(const char* id, const std::vector<std::string>& items, int& currentIdx) {
        if (currentIdx < 0 || currentIdx >= static_cast<int>(items.size()))
            currentIdx = 0;
        const char* preview = items.empty() ? "" : items[static_cast<size_t>(currentIdx)].c_str();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, preview)) {
            for (int i = 0; i < static_cast<int>(items.size()); i++) {
                if (ImGui::Selectable(items[static_cast<size_t>(i)].c_str(), i == currentIdx))
                    currentIdx = i;
            }
            ImGui::EndCombo();
        }
    }
} // namespace

CreateDatabaseDialog& CreateDatabaseDialog::instance() {
    static CreateDatabaseDialog dialog;
    return dialog;
}

bool CreateDatabaseDialog::isPostgres() const {
    if (!db_)
        return false;
    DatabaseType type = db_->getConnectionInfo().type;
    return type == DatabaseType::POSTGRESQL || type == DatabaseType::REDSHIFT;
}

void CreateDatabaseDialog::show(Application* app, std::shared_ptr<DatabaseInterface> db) {
    if (open_ || !db)
        return;
    app_ = app;
    db_ = db;

    nameBuf_[0] = '\0';
    commentBuf_[0] = '\0';
    encodingIdx_ = 0;
    charsetIdx_ = 0;
    statusText_.clear();
    statusIsError_ = false;
    closeRequested_ = false;

    // discard any stale load from a previous dialog
    optionsOp_.cancel();
    loadingOptions_ = false;

    if (isPostgres()) {
        // fallbacks shown (disabled) until the async load lands
        owners_ = {"postgres"};
        ownerIdx_ = 0;
        templates_ = {"template1", "template0"};
        templateIdx_ = 0;
        tablespaces_ = {"pg_default"};
        tablespaceIdx_ = 0;
        startPostgresOptionsLoad();
    } else {
        rebuildCollations();
    }

    open_ = true;
    pendingOpen_ = true;
}

void CreateDatabaseDialog::startPostgresOptionsLoad() {
    loadingOptions_ = true;

    auto db = db_;
    optionsOp_.start([db]() {
        PgOptions opts;
        auto* executor = dynamic_cast<IQueryExecutor*>(db.get());
        if (!executor)
            return opts;

        try {
            auto result = executor->executeQuery("SELECT rolname FROM pg_roles ORDER BY rolname");
            if (result.success()) {
                for (const auto& row : result[0].tableData) {
                    if (!row.empty())
                        opts.owners.push_back(row[0]);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("CreateDatabaseDialog: failed to load pg_roles: {}", e.what());
        }

        try {
            auto result = executor->executeQuery(
                "SELECT datname FROM pg_database WHERE datistemplate ORDER BY datname");
            if (result.success()) {
                opts.templates.push_back("template1");
                for (const auto& row : result[0].tableData) {
                    if (!row.empty() && row[0] != "template1")
                        opts.templates.push_back(row[0]);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("CreateDatabaseDialog: failed to load template databases: {}", e.what());
        }

        try {
            auto result =
                executor->executeQuery("SELECT spcname FROM pg_tablespace ORDER BY spcname");
            if (result.success()) {
                for (const auto& row : result[0].tableData) {
                    if (!row.empty())
                        opts.tablespaces.push_back(row[0]);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("CreateDatabaseDialog: failed to load tablespaces: {}", e.what());
        }

        return opts;
    });
}

void CreateDatabaseDialog::pollPostgresOptions() {
    optionsOp_.check([this](PgOptions opts) {
        loadingOptions_ = false;

        if (!opts.owners.empty()) {
            owners_ = std::move(opts.owners);
            ownerIdx_ = 0;
            for (size_t i = 0; i < owners_.size(); i++) {
                if (owners_[i] == "postgres") {
                    ownerIdx_ = static_cast<int>(i);
                    break;
                }
            }
        }

        if (!opts.templates.empty()) {
            templates_ = std::move(opts.templates);
            templateIdx_ = 0;
        }

        if (!opts.tablespaces.empty()) {
            tablespaces_ = std::move(opts.tablespaces);
            tablespaceIdx_ = 0;
            for (size_t i = 0; i < tablespaces_.size(); i++) {
                if (tablespaces_[i] == "pg_default") {
                    tablespaceIdx_ = static_cast<int>(i);
                    break;
                }
            }
        }
    });
}

void CreateDatabaseDialog::rebuildCollations() {
    const std::string charset = kCharsets[charsetIdx_];
    collationIdx_ = 0;

    if (charset == "utf8mb4") {
        collations_ = {"utf8mb4_unicode_ci", "utf8mb4_0900_ai_ci", "utf8mb4_general_ci",
                       "utf8mb4_bin"};
    } else if (charset == "utf8mb3" || charset == "utf8") {
        collations_ = {"utf8_unicode_ci", "utf8_general_ci", "utf8_bin"};
    } else if (charset == "latin1") {
        collations_ = {"latin1_swedish_ci", "latin1_general_ci", "latin1_bin"};
    } else if (charset == "ascii") {
        collations_ = {"ascii_general_ci", "ascii_bin"};
    } else if (charset == "binary") {
        collations_ = {"binary"};
    } else {
        // server picks the charset's default collation
        collations_.clear();
    }
}

void CreateDatabaseDialog::setStatus(const std::string& text, bool isError) {
    statusText_ = text;
    statusIsError_ = isError;
}

void CreateDatabaseDialog::startCreate() {
    if (nameBuf_[0] == '\0') {
        setStatus("Please enter a database name", true);
        return;
    }

    CreateDatabaseOptions opts;
    opts.name = nameBuf_;
    opts.comment = commentBuf_;

    if (isPostgres()) {
        opts.owner = owners_.empty() ? "" : owners_[static_cast<size_t>(ownerIdx_)];
        opts.templateDb = templates_.empty() ? "" : templates_[static_cast<size_t>(templateIdx_)];
        opts.encoding = kEncodings[encodingIdx_];
        opts.tablespace =
            tablespaces_.empty() ? "" : tablespaces_[static_cast<size_t>(tablespaceIdx_)];
    } else {
        opts.charset = kCharsets[charsetIdx_];
        opts.collation = collations_.empty() ? "" : collations_[static_cast<size_t>(collationIdx_)];
    }

    setStatus("Creating...", false);

    auto db = db_;
    createOp_.start([db, opts]() { return db->createDatabaseWithOptions(opts); });
}

void CreateDatabaseDialog::pollCreate() {
    createOp_.check([this](std::pair<bool, std::string> result) {
        if (result.first) {
            // refresh the sidebar's database list
            if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db_.get())) {
                pgDb->refreshDatabaseNames();
            } else if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db_.get())) {
                mysqlDb->refreshDatabaseNames();
            }
            closeRequested_ = true;
        } else {
            setStatus("Failed: " + result.second, true);
        }
        // dialog dismissed mid-create: drop the reference now that we're done
        if (!open_)
            db_.reset();
    });
}

void CreateDatabaseDialog::finishClose() {
    open_ = false;
    pendingOpen_ = false;
    closeRequested_ = false;
    optionsOp_.cancel();
    loadingOptions_ = false;
    // keep db_ alive while a create is still in flight so pollCreate can refresh
    if (!createOp_.isRunning())
        db_.reset();
}

void CreateDatabaseDialog::render() {
    // keep polling after close so an in-flight create still refreshes the list
    pollCreate();

    if (!open_)
        return;

    pollPostgresOptions();

    if (pendingOpen_) {
        ImGui::OpenPopup(kPopupId);
        pendingOpen_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(kDialogWidth, 0.0f), ImVec2(kDialogWidth, FLT_MAX));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, Application::getInstance().getCurrentColors().overlay1);

    bool stayOpen = true;
    if (!ImGui::BeginPopupModal(kPopupId, &stayOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        // closed externally (e.g. escape)
        finishClose();
        return;
    }

    if (closeRequested_ || !stayOpen) {
        finishClose();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));

    const bool busy = createOp_.isRunning();
    ImGui::BeginDisabled(busy);

    fieldLabel("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##createdb_name", "new_database", nameBuf_, sizeof(nameBuf_));

    if (isPostgres()) {
        // dynamic lists stay disabled until the async load lands
        ImGui::BeginDisabled(loadingOptions_);
        fieldLabel("Owner");
        stringCombo("##createdb_owner", owners_, ownerIdx_);

        fieldLabel("Template");
        stringCombo("##createdb_template", templates_, templateIdx_);
        ImGui::EndDisabled();

        fieldLabel("Encoding");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::Combo("##createdb_encoding", &encodingIdx_, kEncodings, kEncodingCount);

        ImGui::BeginDisabled(loadingOptions_);
        fieldLabel("Tablespace");
        stringCombo("##createdb_tablespace", tablespaces_, tablespaceIdx_);
        ImGui::EndDisabled();
    } else {
        fieldLabel("Charset");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##createdb_charset", &charsetIdx_, kCharsets, kCharsetCount))
            rebuildCollations();

        if (!collations_.empty()) {
            fieldLabel("Collation");
            stringCombo("##createdb_collation", collations_, collationIdx_);
        }
    }

    fieldLabel("Comment");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##createdb_comment", "(optional)", commentBuf_, sizeof(commentBuf_));

    ImGui::EndDisabled();

    ImGui::Separator();

    // status row
    const auto& colors = Application::getInstance().getCurrentColors();
    if (busy) {
        UIUtils::Spinner("##createdb_spinner", 7.0f, 3, ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SameLine();
    }
    if (!statusText_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, statusIsError_ ? colors.red : colors.subtext0);
        ImGui::TextWrapped("%s", statusText_.c_str());
        ImGui::PopStyleColor();
    } else if (!busy) {
        // keep the row height stable
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }

    // buttons, right aligned
    float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(rightEdge - kButtonW * 2 - Theme::Spacing::M);

    bool cancelClicked =
        UIUtils::Button("Cancel##createdb", UIUtils::ButtonVariant::Secondary, ImVec2(kButtonW, 0));
    ImGui::SameLine();

    bool createClicked = false;
    ImGui::BeginDisabled(busy);
    if (UIUtils::Button("Create##createdb", UIUtils::ButtonVariant::Primary, ImVec2(kButtonW, 0)))
        createClicked = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        createClicked = true;
    ImGui::EndDisabled();

    ImGui::PopStyleVar();

    if (cancelClicked) {
        finishClose();
        ImGui::CloseCurrentPopup();
    } else if (createClicked && !busy) {
        startCreate();
    }

    ImGui::EndPopup();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
