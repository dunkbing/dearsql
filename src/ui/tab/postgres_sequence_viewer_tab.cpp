#include "ui/tab/postgres_sequence_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <cstdio>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>
#include <utility>

namespace {
    std::string makeTabName(const std::string& sequenceName) {
        return std::format("{} {}", ICON_FK_SORT_NUMERIC_ASC, sequenceName);
    }

    std::string escapeSqlLiteral(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == '\'') {
                r += "''";
            } else {
                r += c;
            }
        }
        return r;
    }

    std::string escapeSqlIdent(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == '"') {
                r += "\"\"";
            } else {
                r += c;
            }
        }
        return r;
    }

    void writeInt(char* buf, std::size_t bufSize, std::int64_t v) {
        std::snprintf(buf, bufSize, "%lld", static_cast<long long>(v));
    }

    void writeStr(char* buf, std::size_t bufSize, const std::string& s) {
        std::snprintf(buf, bufSize, "%s", s.c_str());
    }

    // std::format treats char[N] as a string_view over the entire array (incl.
    // trailing null bytes); convert to const char* so it reads only up to '\0'.
    const char* cstr(const char* buf) {
        return buf;
    }

    constexpr float kLabelWidth = 120.0f;
    constexpr float kSmallLabelWidth = 100.0f;
    constexpr float kInputWidth = 240.0f;
    constexpr float kNumberWidth = 140.0f;
} // namespace

PostgresSequenceViewerTab::PostgresSequenceViewerTab(PostgresSchemaNode* schema,
                                                     std::string sequenceName)
    : Tab(makeTabName(sequenceName), TabType::POSTGRES_SEQUENCE_VIEWER), schema_(schema),
      sequenceName_(std::move(sequenceName)) {
    writeStr(tableNameBuf_, sizeof(tableNameBuf_), sequenceName_);
    ddlEditor_.SetReadOnly(true);
    ddlEditor_.SetShowLineNumbers(false);
    ddlEditor_.SetText("-- Loading...");
}

IDatabaseNode* PostgresSequenceViewerTab::getDatabaseNode() const {
    return schema_;
}

void PostgresSequenceViewerTab::render() {
    if (!loaded_ && !loadError_ && !fetchOp_.isRunning()) {
        fetchAsync();
    }
    checkFetchStatus();

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    if (!ddlEditorInitialized_) {
        ddlEditor_.SetPalette(dearsql::TextEditor::FromTheme(
            app.isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));
        ddlEditorInitialized_ = true;
    }

    if (fetchOp_.isRunning()) {
        ImGui::TextUnformatted("Loading...");
        ImGui::SameLine(0, Theme::Spacing::S);
        UIUtils::Spinner("##pg_seq_loading", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        return;
    }

    if (loadError_) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("Failed to load sequence: %s", loadErrorMessage_.c_str());
        ImGui::PopStyleColor();
        return;
    }

    renderToolbar();

    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));

    auto textInput = [&](const char* label, const char* id, char* buf, std::size_t bufSize,
                         float labelW = kLabelWidth, float inputW = kInputWidth) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText(id, buf, bufSize)) {
            rebuildDdl();
        }
    };

    auto numberInput = [&](const char* label, const char* id, char* buf, std::size_t bufSize,
                           float labelW = kLabelWidth, float inputW = kNumberWidth) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText(id, buf, bufSize, ImGuiInputTextFlags_CharsDecimal)) {
            rebuildDdl();
        }
    };

    auto disabledNumberInput = [&](const char* label, const char* id, char* buf,
                                   std::size_t bufSize, float labelW = kLabelWidth,
                                   float inputW = kNumberWidth) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::BeginDisabled();
        ImGui::InputText(id, buf, bufSize, ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
    };

    // left column: Table Name, Comment, Last Value
    ImGui::BeginGroup();
    {
        textInput("Table Name:", "##pg_seq_table_name", tableNameBuf_, sizeof(tableNameBuf_));

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Comment:");
        ImGui::SameLine(kLabelWidth);
        ImGui::SetNextItemWidth(kInputWidth);
        if (ImGui::InputTextMultiline("##pg_seq_comment", commentBuf_, sizeof(commentBuf_),
                                      ImVec2(kInputWidth, ImGui::GetTextLineHeight() * 2.5f))) {
            rebuildDdl();
        }

        numberInput("Last Value:", "##pg_seq_last_value", lastValueBuf_, sizeof(lastValueBuf_));
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, Theme::Spacing::XL);

    // middle column: Start value, Min Value, Max Value, Increment By
    ImGui::BeginGroup();
    {
        numberInput("Start value:", "##pg_seq_start", startValueBuf_, sizeof(startValueBuf_));
        numberInput("Min Value:", "##pg_seq_min", minValueBuf_, sizeof(minValueBuf_));
        numberInput("Max Value:", "##pg_seq_max", maxValueBuf_, sizeof(maxValueBuf_));
        numberInput("Increment By:", "##pg_seq_inc", incrementByBuf_, sizeof(incrementByBuf_));
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, Theme::Spacing::XL);

    // right column: Cache, Cycled, Object ID, Owner
    ImGui::BeginGroup();
    {
        numberInput("Cache:", "##pg_seq_cache", cacheBuf_, sizeof(cacheBuf_), kSmallLabelWidth);

        // checkbox aligned with input column above
        ImGui::Indent(kSmallLabelWidth);
        if (ImGui::Checkbox("Cycled##pg_seq_cycled", &cycled_)) {
            rebuildDdl();
        }
        ImGui::Unindent(kSmallLabelWidth);

        disabledNumberInput("Object ID:", "##pg_seq_oid", objectIdBuf_, sizeof(objectIdBuf_),
                            kSmallLabelWidth);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Owner:");
        ImGui::SameLine(kSmallLabelWidth);
        ImGui::TextUnformatted(ownerBuf_);
    }
    ImGui::EndGroup();

    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, Theme::Spacing::L));

    ImGui::TextUnformatted("DDL");
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x > 0 && avail.y > 0) {
        ddlEditor_.Render("##pg_seq_ddl", avail, true);
    }

    showSaveConfirmationDialog();
    checkSQLExecutionStatus();
}

bool PostgresSequenceViewerTab::hasUnsavedChanges() const {
    return isDirty();
}

bool PostgresSequenceViewerTab::isDirty() const {
    if (!loaded_) {
        return false;
    }
    if (sequenceName_ != tableNameBuf_) {
        return true;
    }
    if (original_.comment != commentBuf_) {
        return true;
    }
    if (original_.cycled != cycled_) {
        return true;
    }
    auto fmt = [](std::int64_t v) { return std::format("{}", v); };
    if (fmt(original_.lastValue) != lastValueBuf_) {
        return true;
    }
    if (fmt(original_.startValue) != startValueBuf_) {
        return true;
    }
    if (fmt(original_.minValue) != minValueBuf_) {
        return true;
    }
    if (fmt(original_.maxValue) != maxValueBuf_) {
        return true;
    }
    if (fmt(original_.incrementBy) != incrementByBuf_) {
        return true;
    }
    if (fmt(original_.cacheSize) != cacheBuf_) {
        return true;
    }
    return false;
}

void PostgresSequenceViewerTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const bool dirty = isDirty();

    if (dirty) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
        if (UIUtils::Button(ICON_FA_FLOPPY_DISK " Save", UIUtils::ButtonVariant::Primary)) {
            saveChanges();
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginDisabled();
        UIUtils::Button(ICON_FA_FLOPPY_DISK " Save", UIUtils::ButtonVariant::Primary);
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Save");
    }

    ImGui::SameLine(0, Theme::Spacing::M);
    if (dirty) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        if (UIUtils::Button(ICON_FA_XMARK " Discard", UIUtils::ButtonVariant::Danger)) {
            cancelChanges();
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginDisabled();
        UIUtils::Button(ICON_FA_XMARK " Discard", UIUtils::ButtonVariant::Danger);
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Discard changes");
    }

    if (dirty) {
        ImGui::SameLine(0, Theme::Spacing::L);
        ImGui::TextColored(colors.peach, "Unsaved changes");
    }
}

void PostgresSequenceViewerTab::saveChanges() {
    if (!isDirty()) {
        return;
    }
    pendingUpdateSQL_ = generateUpdateSQL();
    if (pendingUpdateSQL_.empty()) {
        return;
    }
    showSaveDialog_ = true;
}

void PostgresSequenceViewerTab::cancelChanges() {
    writeStr(tableNameBuf_, sizeof(tableNameBuf_), sequenceName_);
    writeStr(commentBuf_, sizeof(commentBuf_), original_.comment);
    writeInt(lastValueBuf_, sizeof(lastValueBuf_), original_.lastValue);
    writeInt(startValueBuf_, sizeof(startValueBuf_), original_.startValue);
    writeInt(minValueBuf_, sizeof(minValueBuf_), original_.minValue);
    writeInt(maxValueBuf_, sizeof(maxValueBuf_), original_.maxValue);
    writeInt(incrementByBuf_, sizeof(incrementByBuf_), original_.incrementBy);
    writeInt(cacheBuf_, sizeof(cacheBuf_), original_.cacheSize);
    cycled_ = original_.cycled;
    rebuildDdl();
}

std::vector<std::string> PostgresSequenceViewerTab::generateUpdateSQL() const {
    std::vector<std::string> stmts;
    if (!schema_) {
        return stmts;
    }

    const std::string schemaName = schema_->name;
    const std::string oldName = sequenceName_;
    const std::string newName = tableNameBuf_;
    const std::string targetName = newName.empty() ? oldName : newName;

    if (oldName != newName && !newName.empty()) {
        stmts.push_back(std::format("ALTER SEQUENCE \"{}\".\"{}\" RENAME TO \"{}\";",
                                    escapeSqlIdent(schemaName), escapeSqlIdent(oldName),
                                    escapeSqlIdent(newName)));
    }

    auto fmt = [](std::int64_t v) { return std::format("{}", v); };

    std::string alter;
    auto addOpt = [&](const std::string& opt) {
        if (alter.empty()) {
            alter = std::format("ALTER SEQUENCE \"{}\".\"{}\"", escapeSqlIdent(schemaName),
                                escapeSqlIdent(targetName));
        }
        alter += "\n    " + opt;
    };

    if (fmt(original_.startValue) != startValueBuf_) {
        addOpt(std::format("START WITH {}", cstr(startValueBuf_)));
    }
    if (fmt(original_.minValue) != minValueBuf_) {
        addOpt(std::format("MINVALUE {}", cstr(minValueBuf_)));
    }
    if (fmt(original_.maxValue) != maxValueBuf_) {
        addOpt(std::format("MAXVALUE {}", cstr(maxValueBuf_)));
    }
    if (fmt(original_.incrementBy) != incrementByBuf_) {
        addOpt(std::format("INCREMENT BY {}", cstr(incrementByBuf_)));
    }
    if (fmt(original_.cacheSize) != cacheBuf_) {
        addOpt(std::format("CACHE {}", cstr(cacheBuf_)));
    }
    if (original_.cycled != cycled_) {
        addOpt(cycled_ ? "CYCLE" : "NO CYCLE");
    }
    if (!alter.empty()) {
        stmts.push_back(alter + ";");
    }

    if (fmt(original_.lastValue) != lastValueBuf_) {
        // setval resets the sequence's current value
        stmts.push_back(std::format("SELECT setval('\"{}\".\"{}\"', {});",
                                    escapeSqlIdent(schemaName), escapeSqlIdent(targetName),
                                    cstr(lastValueBuf_)));
    }

    if (original_.comment != commentBuf_) {
        if (commentBuf_[0] == '\0') {
            stmts.push_back(std::format("COMMENT ON SEQUENCE \"{}\".\"{}\" IS NULL;",
                                        escapeSqlIdent(schemaName), escapeSqlIdent(targetName)));
        } else {
            stmts.push_back(std::format("COMMENT ON SEQUENCE \"{}\".\"{}\" IS '{}';",
                                        escapeSqlIdent(schemaName), escapeSqlIdent(targetName),
                                        escapeSqlLiteral(commentBuf_)));
        }
    }

    return stmts;
}

void PostgresSequenceViewerTab::showSaveConfirmationDialog() {
    if (!showSaveDialog_) {
        return;
    }

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();
    const bool dark = app.isDarkTheme();

    if (!dialogOpened_) {
        std::string combined;
        for (size_t i = 0; i < pendingUpdateSQL_.size(); i++) {
            if (i > 0) {
                combined += "\n\n";
            }
            combined += pendingUpdateSQL_[i];
        }
        saveDialogEditor_.SetLanguage(dearsql::TextEditor::Language::SQL);
        saveDialogEditor_.SetShowLineNumbers(true);
        saveDialogEditor_.SetText(combined);

        ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Always);
        ImGui::OpenPopup("Confirm Update Sequence");
        dialogOpened_ = true;
    }

    saveDialogEditor_.SetPalette(
        dearsql::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::CornerRadius::MEDIUM);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.base);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface2);

    if (ImGui::BeginPopupModal("Confirm Update Sequence", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextUnformatted("Review and edit the SQL before executing.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

        const float footerH = ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::M * 2;
        const float editorH = ImGui::GetContentRegionAvail().y - footerH;
        saveDialogEditor_.Render("##pg_seq_save_editor", ImVec2(-1, editorH), true);

        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(Theme::Spacing::L, Theme::Spacing::S));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        if (sqlExecutionOp_.isRunning()) {
            ImGui::BeginDisabled();
            UIUtils::Button(ICON_FA_PLAY " Execute", UIUtils::ButtonVariant::Primary);
            ImGui::EndDisabled();

            ImGui::SameLine(0, Theme::Spacing::M);
            UIUtils::Spinner("##pg_seq_exec_spinner", 7.0f, 2, ImGui::GetColorU32(colors.blue));
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextUnformatted("Executing...");
            ImGui::PopStyleColor();
        } else {
            if (UIUtils::Button(ICON_FA_PLAY " Execute", UIUtils::ButtonVariant::Primary)) {
                const std::string editedSQL = saveDialogEditor_.GetText();
                auto* schema = schema_;
                sqlExecutionOp_.start([schema, editedSQL]() -> std::pair<bool, std::string> {
                    if (!schema) {
                        return {false, "Schema not available"};
                    }
                    spdlog::debug("Executing sequence update SQL: {}", editedSQL);
                    const auto result = schema->executeQuery(editedSQL);
                    if (!result.success()) {
                        return {false, result.errorMessage()};
                    }
                    return {true, {}};
                });
            }

            ImGui::SameLine(0, Theme::Spacing::M);

            if (UIUtils::Button(ICON_FA_XMARK " Cancel")) {
                showSaveDialog_ = false;
                pendingUpdateSQL_.clear();
                dialogOpened_ = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopStyleVar(3);
        ImGui::EndPopup();
    } else if (!ImGui::IsPopupOpen("Confirm Update Sequence")) {
        showSaveDialog_ = false;
        pendingUpdateSQL_.clear();
        dialogOpened_ = false;
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

void PostgresSequenceViewerTab::checkSQLExecutionStatus() {
    sqlExecutionOp_.check([this](std::pair<bool, std::string> result) {
        auto [success, errorMessage] = result;
        if (success) {
            // pick up the (possibly renamed) sequence and refresh the schema's sequence list
            const std::string newName = tableNameBuf_;
            if (!newName.empty() && newName != sequenceName_) {
                sequenceName_ = newName;
                setName(std::format("{} {}", ICON_FK_SORT_NUMERIC_ASC, sequenceName_));
            }
            if (schema_) {
                schema_->startSequencesLoadAsync(true);
            }
            showSaveDialog_ = false;
            pendingUpdateSQL_.clear();
            dialogOpened_ = false;
            loaded_ = false;
            loadError_ = false;
            fetchAsync();
        } else {
            spdlog::error("Sequence update failed: {}", errorMessage);
        }
    });
}

void PostgresSequenceViewerTab::fetchAsync() {
    if (!schema_) {
        loadError_ = true;
        loadErrorMessage_ = "Schema not available";
        return;
    }

    auto* schema = schema_;
    const std::string seqName = sequenceName_;
    const std::string schemaName = schema_->name;

    fetchOp_.start([schema, seqName, schemaName]() -> FetchResult {
        FetchResult r;
        try {
            const std::string sql = std::format(
                "SELECT s.start_value::text, s.min_value::text, s.max_value::text, "
                "s.increment_by::text, s.cycle, s.cache_size::text, s.last_value::text, "
                "c.oid::text, pg_catalog.pg_get_userbyid(c.relowner) AS owner, "
                "s.data_type::text, "
                "COALESCE(pg_catalog.obj_description(c.oid, 'pg_class'), '') AS comment "
                "FROM pg_catalog.pg_sequences s "
                "JOIN pg_catalog.pg_class c ON c.relname = s.sequencename "
                "JOIN pg_catalog.pg_namespace n "
                "  ON n.oid = c.relnamespace AND n.nspname = s.schemaname "
                "WHERE s.schemaname = '{}' AND s.sequencename = '{}'",
                escapeSqlLiteral(schemaName), escapeSqlLiteral(seqName));

            const QueryResult result = schema->executeQuery(sql);
            if (!result.success()) {
                r.ok = false;
                r.error = result.errorMessage();
                return r;
            }
            if (result.empty() || result[0].tableData.empty()) {
                r.ok = false;
                r.error = "Sequence not found";
                return r;
            }
            const auto& row = result[0].tableData[0];
            auto toI64 = [](const std::string& s) -> std::int64_t {
                try {
                    return std::stoll(s);
                } catch (...) {
                    return 0;
                }
            };
            if (row.size() >= 11) {
                r.startValue = toI64(row[0]);
                r.minValue = toI64(row[1]);
                r.maxValue = toI64(row[2]);
                r.incrementBy = toI64(row[3]);
                r.cycled = (row[4] == "t" || row[4] == "true" || row[4] == "1");
                r.cacheSize = toI64(row[5]);
                r.lastValue = toI64(row[6]);
                r.objectId = toI64(row[7]);
                r.owner = row[8];
                r.dataType = row[9];
                r.comment = row[10];
            }
            r.ok = true;
            return r;
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
            return r;
        }
    });
}

void PostgresSequenceViewerTab::checkFetchStatus() {
    fetchOp_.check([this](FetchResult r) {
        if (!r.ok) {
            loadError_ = true;
            loadErrorMessage_ = r.error;
            return;
        }
        dataType_ = r.dataType;
        writeStr(commentBuf_, sizeof(commentBuf_), r.comment);
        writeInt(lastValueBuf_, sizeof(lastValueBuf_), r.lastValue);
        writeInt(startValueBuf_, sizeof(startValueBuf_), r.startValue);
        writeInt(minValueBuf_, sizeof(minValueBuf_), r.minValue);
        writeInt(maxValueBuf_, sizeof(maxValueBuf_), r.maxValue);
        writeInt(incrementByBuf_, sizeof(incrementByBuf_), r.incrementBy);
        writeInt(cacheBuf_, sizeof(cacheBuf_), r.cacheSize);
        cycled_ = r.cycled;
        writeInt(objectIdBuf_, sizeof(objectIdBuf_), r.objectId);
        writeStr(ownerBuf_, sizeof(ownerBuf_), r.owner);
        original_ = std::move(r);
        loaded_ = true;
        rebuildDdl();
    });
}

void PostgresSequenceViewerTab::rebuildDdl() {
    if (!schema_) {
        return;
    }
    const std::string schemaName = schema_->name;
    const std::string seqName = tableNameBuf_;

    std::string ddl;
    ddl += std::format("CREATE SEQUENCE \"{}\".\"{}\"\n", escapeSqlIdent(schemaName),
                       escapeSqlIdent(seqName));
    if (!dataType_.empty()) {
        ddl += std::format("    AS {}\n", dataType_);
    }
    ddl += std::format("    START WITH {}\n", cstr(startValueBuf_));
    ddl += std::format("    INCREMENT BY {}\n", cstr(incrementByBuf_));
    ddl += std::format("    MINVALUE {}\n", cstr(minValueBuf_));
    ddl += std::format("    MAXVALUE {}\n", cstr(maxValueBuf_));
    ddl += std::format("    CACHE {}\n", cstr(cacheBuf_));
    ddl += cycled_ ? "    CYCLE;\n" : "    NO CYCLE;\n";

    if (ownerBuf_[0] != '\0') {
        ddl += std::format("\nALTER SEQUENCE \"{}\".\"{}\" OWNER TO \"{}\";\n",
                           escapeSqlIdent(schemaName), escapeSqlIdent(seqName),
                           escapeSqlIdent(ownerBuf_));
    }

    if (commentBuf_[0] != '\0') {
        ddl += std::format("\nCOMMENT ON SEQUENCE \"{}\".\"{}\" IS '{}';\n",
                           escapeSqlIdent(schemaName), escapeSqlIdent(seqName),
                           escapeSqlLiteral(commentBuf_));
    }

    ddlEditor_.SetText(ddl);
}
