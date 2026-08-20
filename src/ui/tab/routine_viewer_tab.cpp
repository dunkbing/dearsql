#include "ui/tab/routine_viewer_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db_interface.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <format>

static std::string makeTabName(const Routine& routine) {
    const bool isProc = routine.kind == RoutineKind::Procedure;
    return std::format("{} {}", isProc ? ICON_FA_GEAR : ICON_FA_CODE, routine.name);
}

RoutineViewerTab::RoutineViewerTab(IDatabaseNode* node, const Routine& routine)
    : Tab(makeTabName(routine), TabType::ROUTINE_VIEWER), node_(node), routine_(routine) {}

void RoutineViewerTab::render() {
    const bool dark = Application::getInstance().isDarkTheme();
    editor_.SetPalette(
        dearsql::TextEditor::FromTheme(dark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT));

    if (!definitionLoaded_ && !fetchOp_.isRunning() && !loadError_) {
        fetchDefinitionAsync();
    }

    checkFetchStatus();
    checkSaveStatus();

    // track edits after initial load
    if (definitionLoaded_ && editor_.IsContentDirty()) {
        contentModified_ = true;
        editor_.ClearContentDirty();
    }

    // Cmd+S / Ctrl+S
    const bool wantSave = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
                          (ImGui::GetIO().KeyMods & ImGuiMod_Shortcut) &&
                          ImGui::IsKeyPressed(ImGuiKey_S, false);

    const auto& colors = Application::getInstance().getCurrentColors();

    // header: kind badge + signature + return type
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Theme::Spacing::M - Theme::Spacing::S);
    ImGui::AlignTextToFramePadding();

    const bool isProc = routine_.kind == RoutineKind::Procedure;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(isProc ? colors.peach : colors.yellow));
    ImGui::TextUnformatted(isProc ? ICON_FA_GEAR " PROCEDURE" : ICON_FA_CODE " FUNCTION");
    ImGui::PopStyleColor();

    ImGui::SameLine(0, Theme::Spacing::L);
    ImGui::TextUnformatted(routine_.signature.c_str());

    if (!routine_.returnType.empty() && !isProc) {
        ImGui::SameLine(0, Theme::Spacing::L);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::Text("→ %s", routine_.returnType.c_str());
        ImGui::PopStyleColor();
    }

    // save button inline with header once definition is loaded
    if (definitionLoaded_ && !loadError_) {
        ImGui::SameLine(0, Theme::Spacing::L);
        renderToolbar();
    }

    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));

    if (fetchOp_.isRunning()) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float cx = ImGui::GetWindowPos().x + available.x * 0.5f;
        const float cy = ImGui::GetWindowPos().y + ImGui::GetCursorPosY() + available.y * 0.5f;
        constexpr float r = 10.0f;
        ImGui::SetCursorScreenPos(ImVec2(cx - r, cy - r - Theme::Spacing::M));
        UIUtils::Spinner("##routine_loading", r, 2, ImGui::GetColorU32(ImGuiCol_Text));
        const char* msg = "Loading definition...";
        const float tw = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorScreenPos(ImVec2(cx - tw * 0.5f, cy + r + Theme::Spacing::S));
        ImGui::Text("%s", msg);
        return;
    }

    if (loadError_) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("%s", loadErrorMessage_.c_str());
        ImGui::PopStyleColor();
        return;
    }

    if (wantSave && !saveOp_.isRunning()) {
        saveDefinitionAsync();
    }

    editor_.Render("##routine_def", ImGui::GetContentRegionAvail(), true);
}

void RoutineViewerTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const bool saving = saveOp_.isRunning();

    if (saving)
        ImGui::BeginDisabled();

    if (UIUtils::Button(ICON_FA_FLOPPY_DISK " Save", UIUtils::ButtonVariant::Primary)) {
        saveDefinitionAsync();
    }

    if (saving)
        ImGui::EndDisabled();

    if (saving) {
        ImGui::SameLine(0, Theme::Spacing::M);
        UIUtils::Spinner("##routine_saving", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::TextUnformatted("Saving...");
    } else if (saveFeedbackTimer_ > 0.0f) {
        saveFeedbackTimer_ -= ImGui::GetIO().DeltaTime;
        ImGui::SameLine(0, Theme::Spacing::M);
        if (saveSuccess_) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
            ImGui::TextUnformatted(ICON_FA_CHECK " Saved");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
            ImGui::TextUnformatted(saveMessage_.c_str());
            ImGui::PopStyleColor();
        }
    } else if (contentModified_) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        ImGui::TextUnformatted(ICON_FA_CIRCLE "  unsaved");
        ImGui::PopStyleColor();
    }
}

void RoutineViewerTab::fetchDefinitionAsync() {
    auto* node = node_;
    const Routine routine = routine_;
    const std::string query = buildDefinitionQuery();
    const DatabaseType dbType = node_ ? node_->getDatabaseType() : DatabaseType::SQLITE;

    if (query.empty()) {
        editor_.SetText("-- Definition not available for this database type.");
        definitionLoaded_ = true;
        return;
    }

    fetchOp_.start([node, routine, query, dbType]() -> std::string {
        try {
            const QueryResult result = node->executeQuery(query);
            if (!result.success() || result.empty()) {
                const std::string err = result.errorMessage();
                return err.empty() ? "-- Definition not found." : "-- Error: " + err;
            }

            const auto& stmt = result[0];
            if (stmt.tableData.empty()) {
                return "-- Definition not found.";
            }

            // MySQL/MariaDB: SHOW CREATE returns definition in 3rd column (index 2).
            // Strip the DEFINER clause (requires SUPER/SET_USER_ID to recreate) and
            // prepend DROP so the content round-trips cleanly on save.
            if (dbType == DatabaseType::MYSQL || dbType == DatabaseType::MARIADB) {
                const auto& row = stmt.tableData[0];
                std::string def = (row.size() >= 3) ? row[2] : (row.empty() ? "" : row[0]);
                if (def.empty())
                    return "-- Definition not found.";

                // remove DEFINER=`user`@`host` if present
                const std::string definerKw = "DEFINER=";
                if (const auto defPos = def.find(definerKw); defPos != std::string::npos) {
                    size_t p = defPos + definerKw.size();
                    // skip two backtick-quoted segments: `user`@`host`
                    for (int seg = 0; seg < 2; ++seg) {
                        if (p < def.size() && def[p] == '`') {
                            ++p;
                            while (p < def.size() && def[p] != '`')
                                ++p;
                            if (p < def.size())
                                ++p;
                        }
                        if (seg == 0 && p < def.size() && def[p] == '@')
                            ++p;
                    }
                    while (p < def.size() && def[p] == ' ')
                        ++p;
                    def.erase(defPos, p - defPos);
                }

                const std::string_view kind =
                    routine.kind == RoutineKind::Procedure ? "PROCEDURE" : "FUNCTION";
                return std::format("DROP {} IF EXISTS `{}`;\n{}", kind, routine.name, def);
            }

            // Oracle: concatenate TEXT lines
            if (dbType == DatabaseType::ORACLE) {
                std::string def;
                for (const auto& row : stmt.tableData) {
                    if (!row.empty())
                        def += row[0];
                }
                return def.empty() ? "-- Definition not found." : def;
            }

            // MSSQL: OBJECT_DEFINITION returns CREATE ...; rewrite to CREATE OR ALTER
            // so the definition can be saved without a prior DROP (SQL Server 2016+).
            if (dbType == DatabaseType::MSSQL) {
                const auto& row = stmt.tableData[0];
                std::string def = row.empty() ? "" : row[0];
                if (def.empty())
                    return "-- Definition not found.";
                constexpr std::string_view createKw = "CREATE ";
                if (def.starts_with(createKw))
                    def = "CREATE OR ALTER " + def.substr(createKw.size());
                return def;
            }

            // PostgreSQL / others: first row, first column
            const auto& row = stmt.tableData[0];
            return row.empty() ? "-- Definition not found." : row[0];
        } catch (const std::exception& e) {
            return std::string("-- Error fetching definition: ") + e.what();
        }
    });
}

void RoutineViewerTab::checkFetchStatus() {
    if (!fetchOp_.isRunning()) {
        return;
    }
    fetchOp_.check([this](std::string definition) {
        if (definition.starts_with("-- Error:")) {
            loadError_ = true;
            loadErrorMessage_ = definition.substr(3);
        } else {
            editor_.SetText(definition);
            definitionLoaded_ = true;
        }
    });
}

void RoutineViewerTab::saveDefinitionAsync() {
    if (saveOp_.isRunning() || !node_) {
        return;
    }

    auto* node = node_;
    const std::string sql = editor_.GetText();

    saveOp_.start([node, sql]() -> std::pair<bool, std::string> {
        try {
            const QueryResult result = node->executeQuery(sql);
            if (!result.success()) {
                return {false, result.errorMessage()};
            }
            return {true, {}};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    });
}

void RoutineViewerTab::checkSaveStatus() {
    if (!saveOp_.isRunning()) {
        return;
    }
    saveOp_.check([this](std::pair<bool, std::string> outcome) {
        saveSuccess_ = outcome.first;
        saveFeedbackTimer_ = 3.0f;
        if (saveSuccess_) {
            contentModified_ = false;
            saveMessage_.clear();
        } else {
            saveMessage_ = std::format("{} {}", ICON_FA_TRIANGLE_EXCLAMATION, outcome.second);
        }
    });
}

std::string RoutineViewerTab::buildDefinitionQuery() const {
    if (!node_) {
        return {};
    }

    const std::string& name = routine_.name;
    const DatabaseType dbType = node_->getDatabaseType();

    const std::string schema = node_->getName();

    switch (dbType) {
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT: {
        // signature is "func_name(arg1_type, arg2_type)" from pg_get_function_identity_arguments.
        // Use regprocedure cast with schema + args for an exact match on overloaded routines.
        const auto lparen = routine_.signature.find('(');
        const auto rparen = routine_.signature.rfind(')');
        if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen) {
            const std::string args = routine_.signature.substr(lparen + 1, rparen - lparen - 1);
            return std::format("SELECT pg_get_functiondef('\"{}\".\"{}\"{}'::regprocedure)", schema,
                               name, args.empty() ? "()" : std::format("({})", args));
        }
        // fallback for zero-arg routines with no parens in signature
        return std::format("SELECT pg_get_functiondef(p.oid)"
                           " FROM pg_proc p"
                           " JOIN pg_namespace n ON n.oid = p.pronamespace"
                           " WHERE n.nspname = '{}' AND p.proname = '{}'"
                           " LIMIT 1",
                           schema, name);
    }

    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        if (routine_.kind == RoutineKind::Procedure) {
            return std::format("SHOW CREATE PROCEDURE `{}`", name);
        }
        return std::format("SHOW CREATE FUNCTION `{}`", name);

    case DatabaseType::MSSQL:
        // qualify with schema so OBJECT_ID resolves correctly in non-default schemas
        return std::format("SELECT OBJECT_DEFINITION(OBJECT_ID(N'{}.{}'))", schema, name);

    case DatabaseType::ORACLE:
        // filter by OWNER to avoid picking up same-named routines from other visible schemas
        return std::format("SELECT TEXT FROM ALL_SOURCE"
                           " WHERE OWNER = UPPER('{}')"
                           " AND NAME = UPPER('{}')"
                           " AND TYPE IN ('FUNCTION', 'PROCEDURE', 'PACKAGE')"
                           " ORDER BY TYPE, LINE",
                           schema, name);

    default:
        return {};
    }
}
