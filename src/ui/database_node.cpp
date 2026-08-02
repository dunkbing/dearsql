#include "ui/database_node.hpp"
#include "IconsFontAwesome6.h"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "database/cassandra.hpp"
#include "database/database_node.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "platform/alert.hpp"
#include "ui/input_dialog.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/table_editor_tab.hpp"
#include "ui/tab_manager.hpp"

#include "utils/file_dialog.hpp"
#include "utils/spinner.hpp"
#include "utils/table_exporter.hpp"
#include "utils/table_importer.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {
    constexpr const char* CREATE_TABLE_LABEL = "Create Table";
    constexpr const char* REFRESH_LABEL = "Refresh";
    constexpr const char* DELETE_LABEL = "Delete";
    constexpr const char* RENAME_LABEL = "Rename";
    constexpr const char* VIEW_DATA_LABEL = "View Data";
    constexpr const char* EDIT_TABLE_LABEL = "Edit Table";
    constexpr const char* TRUNCATE_LABEL = "Truncate";
    constexpr const char* NEW_SQL_EDITOR_LABEL = "New SQL Editor";
    constexpr const char* NEW_QUERY_EDITOR_LABEL = "New Query Editor";
    constexpr const char* SHOW_DIAGRAM_LABEL = "Show Diagram";
    constexpr const char* LOADING_LABEL = "  Loading...";
    constexpr const char* BACKUP_LABEL = "Backup";
    constexpr const char* RESTORE_LABEL = "Restore";

    // shared routine rendering for all database types
    void renderRoutineItems(const std::vector<Routine>& routines, IDatabaseNode* node) {
        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();
        constexpr ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

        for (const auto& routine : routines) {
            const std::string itemId =
                std::format("routine_{}_{:p}", routine.name, static_cast<const void*>(&routine));
            const std::string label = std::format("   {}###{}", routine.signature, itemId);
            ImGui::TreeNodeEx(label.c_str(), flags);

            const bool isProc = routine.kind == RoutineKind::Procedure;
            const auto iconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(
                iconPos, ImGui::GetColorU32(isProc ? colors.peach : colors.yellow),
                isProc ? ICON_FA_GEAR : ICON_FA_CODE);

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s %s", isProc ? "PROCEDURE" : "FUNCTION",
                                  routine.returnType.c_str());

                if (ImGui::IsMouseDoubleClicked(0)) {
                    app.getTabManager()->createRoutineViewerTab(node, routine);
                }
            }
        }
    }

    std::string sanitizeBackupFileName(std::string name) {
        for (char& c : name) {
            const auto uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '-' && c != '_') {
                c = '_';
            }
        }
        return name.empty() ? "postgres_database" : name;
    }

    std::string timestampForFileName() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        return std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    std::string defaultPostgresBackupName(const std::string& databaseName,
                                          const PostgresBackupFormat format) {
        return std::format("{}_{}.{}", sanitizeBackupFileName(databaseName), timestampForFileName(),
                           format == PostgresBackupFormat::Custom ? "dump" : "sql");
    }

    bool isPlainSqlBackupPath(const std::string& path) {
        auto ext = std::filesystem::path(path).extension().string();
        std::ranges::transform(ext, ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".sql";
    }

    std::string summarizeToolFailure(const PostgresToolResult& result) {
        if (!result.output.empty()) {
            constexpr size_t maxLen = 1800;
            if (result.output.size() <= maxLen) {
                return result.output;
            }
            return result.output.substr(result.output.size() - maxLen);
        }
        return result.message;
    }

    std::string formatBytes(const std::uint64_t bytes) {
        return formatByteSize(static_cast<int64_t>(bytes));
    }

    std::string formatDuration(const double seconds) {
        if (seconds > 60.0 * 60.0 * 24.0) {
            return "--";
        }
        const auto total = static_cast<int>(seconds);
        if (total >= 3600) {
            return std::format("{}h {}m", total / 3600, total % 3600 / 60);
        }
        if (total >= 60) {
            return std::format("{}m {}s", total / 60, total % 60);
        }
        return std::format("{}s", total);
    }

    // ImGui positions a ProgressBar overlay just past the fill and clamps it to
    // the bar, so the label slides along with the fill and ends up squashed
    // against the right edge. Draw the bar with no overlay and center the label
    // over the whole width instead -- twice, clipped to each side of the fill
    // boundary, because one color is unreadable against the other half.
    void progressBarWithCenteredLabel(const float fraction, const float width,
                                     const std::string& label) {
        const ImVec4& onFillColor = Application::getInstance().getCurrentColors().crust;
        const ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImGui::ProgressBar(fraction, ImVec2(width, 0.0f), "");

        const ImVec2 barSize = ImGui::GetItemRectSize();
        const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 labelPos(barPos.x + (barSize.x - labelSize.x) * 0.5f,
                              barPos.y + (barSize.y - labelSize.y) * 0.5f);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 onTrack = ImGui::GetColorU32(ImGuiCol_Text);

        if (fraction < 0.0f) {
            // Indeterminate: the fill sweeps, so there is no stable split.
            draw->AddText(labelPos, onTrack, label.c_str());
            return;
        }

        const ImU32 onFill = ImGui::GetColorU32(onFillColor);
        const float fillX = barPos.x + barSize.x * std::min(fraction, 1.0f);
        const float barBottom = barPos.y + barSize.y;

        draw->PushClipRect(barPos, ImVec2(fillX, barBottom), true);
        draw->AddText(labelPos, onFill, label.c_str());
        draw->PopClipRect();

        draw->PushClipRect(ImVec2(fillX, barPos.y), ImVec2(barPos.x + barSize.x, barBottom), true);
        draw->AddText(labelPos, onTrack, label.c_str());
        draw->PopClipRect();
    }

    // Returns true when Cancel was pressed. widestStatus sizes the panel: the
    // window auto-resizes to its content, so measuring the live status text would
    // make it twitch whenever a digit or unit changed.
    bool renderProgressPanel(const char* id, const char* title, const float fraction,
                             const std::string& overlay, const std::string& status,
                             const char* widestStatus, const bool cancelling) {
        // Anchored to the docked tab area rather than the viewport. Centering on the
        // whole window puts the panel a sidebar's width left of the content it
        // reports on. Falls back to the viewport before the layout exists or when
        // the sidebar is hidden, where the two are the same thing anyway.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 areaPos = viewport->WorkPos;
        ImVec2 areaSize = viewport->WorkSize;
        if (const ImGuiID centerId = Application::getInstance().getCenterDockId(); centerId != 0) {
            if (const ImGuiDockNode* node = ImGui::DockBuilderGetNode(centerId)) {
                areaPos = node->Pos;
                areaSize = node->Size;
            }
        }

        ImGui::SetNextWindowPos(
            ImVec2(areaPos.x + areaSize.x * 0.5f, areaPos.y + areaSize.y - Theme::Spacing::L),
            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.92f);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

        constexpr float minBarWidth = 320.0f;
        const float contentWidth = std::max(minBarWidth, ImGui::CalcTextSize(widestStatus).x);

        bool cancelPressed = false;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(Theme::Spacing::L, Theme::Spacing::L));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));

        if (ImGui::Begin(id, nullptr, flags)) {
            ImGui::TextUnformatted(title);

            // Cursor positions are window-relative, so the row's start has to be
            // carried across to land the button flush with the bar's right edge.
            const float contentStartX = ImGui::GetCursorPosX();
            progressBarWithCenteredLabel(fraction, contentWidth, overlay);
            ImGui::TextUnformatted(status.c_str());

            const float cancelWidth = ImGui::CalcTextSize("Cancel").x +
                                      ImGui::GetStyle().FramePadding.x * 2.0f + Theme::Spacing::M;
            ImGui::SetCursorPosX(contentStartX + contentWidth - cancelWidth);
            ImGui::BeginDisabled(cancelling);
            cancelPressed = ImGui::Button("Cancel", ImVec2(cancelWidth, 0.0f));
            ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        return cancelPressed;
    }

    bool ensurePostgresToolsAvailable(const std::vector<std::string>& toolNames) {
        const PostgresToolResult result = PostgresBackupService::checkToolsAvailable(toolNames);
        if (result.success) {
            return true;
        }
        if (result.output.empty()) {
            Alert::show("PostgreSQL Tools Missing", result.message);
        } else {
            Alert::show("PostgreSQL Tools Missing",
                        std::format("{}\n\n{}", result.message, summarizeToolFailure(result)));
        }
        return false;
    }
} // namespace

DatabaseHierarchy::DatabaseHierarchy(std::shared_ptr<DatabaseInterface> dbInterface)
    : db(std::move(dbInterface)) {}

void DatabaseHierarchy::handleTableClick(const Table* table) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        if (selectedTables_.count(table)) {
            selectedTables_.erase(table);
        } else {
            selectedTables_.insert(table);
        }
        lastAnchorTable_ = table;
    } else if (io.KeyShift && lastAnchorTable_) {
        const auto anchorIt = std::ranges::find(prevVisibleTables_, lastAnchorTable_);
        const auto currentIt = std::ranges::find(prevVisibleTables_, table);
        if (anchorIt != prevVisibleTables_.end() && currentIt != prevVisibleTables_.end()) {
            selectedTables_.clear();
            const auto [first, last] = anchorIt < currentIt ? std::pair{anchorIt, currentIt}
                                                            : std::pair{currentIt, anchorIt};
            for (auto it = first; it <= last; ++it) {
                selectedTables_.insert(*it);
            }
        } else {
            selectedTables_.clear();
            selectedTables_.insert(table);
            lastAnchorTable_ = table;
        }
    } else {
        selectedTables_.clear();
        selectedTables_.insert(table);
        lastAnchorTable_ = table;
    }
}

void DatabaseHierarchy::renderSchemaFilterBadge(const std::string& dbName,
                                                std::vector<std::string> schemaNames,
                                                const ImVec2& nodeMin, const ImVec2& nodeMax,
                                                const void* popupKey) {
    if (schemaNames.empty())
        return;

    const auto& colors = Application::getInstance().getCurrentColors();

    const int total = static_cast<int>(schemaNames.size());
    int hiddenCount = 0;
    for (const auto& name : schemaNames) {
        if (isSchemaHidden(dbName, name))
            ++hiddenCount;
    }
    const int visibleCount = total - hiddenCount;

    const std::string countStr =
        hiddenCount > 0 ? std::format("{}/{}", visibleCount, total) : std::to_string(total);
    const ImVec4& badgeColor = hiddenCount > 0 ? colors.peach : colors.overlay1;

    const float textW = ImGui::CalcTextSize(countStr.c_str()).x;
    constexpr float hPad = 4.0f;
    const float btnW = textW + hPad * 2.0f;
    const float rowH = nodeMax.y - nodeMin.y;
    const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float btnX = rightEdge - btnW - hPad;

    const ImVec2 textPos(btnX + hPad, nodeMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(badgeColor), countStr.c_str());

    const std::string popupId = std::format("##schema_filter_popup_{:p}", popupKey);
    const ImVec2 badgeMin(btnX, nodeMin.y);
    const ImVec2 badgeMax(btnX + btnW, nodeMax.y);
    const bool badgeHovered = ImGui::IsMouseHoveringRect(badgeMin, badgeMax);

    if (badgeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::OpenPopup(popupId.c_str());
    }

    if (badgeHovered) {
        ImGui::GetWindowDrawList()->AddRectFilled(badgeMin, badgeMax,
                                                  ImGui::GetColorU32(colors.surface2), 3.0f);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(badgeColor),
                                            countStr.c_str());
    }

    if (ImGui::BeginPopup(popupId.c_str())) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        std::sort(schemaNames.begin(), schemaNames.end());
        for (const auto& name : schemaNames) {
            bool visible = !isSchemaHidden(dbName, name);
            if (ImGui::Checkbox(name.c_str(), &visible)) {
                setSchemaHidden(dbName, name, !visible);
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMultiSelectMenuContent(
    ITableDataProvider* provider, const std::vector<Table>& nodeTables,
    std::function<void(const std::string&)> dropOne, DatabaseType dbType) {
    std::vector<const Table*> selectedNodeTables;
    std::vector<std::string> selectedNames;
    for (const auto& t : nodeTables) {
        if (selectedTables_.count(&t)) {
            selectedNodeTables.push_back(&t);
            selectedNames.push_back(t.name);
        }
    }

    if (ImGui::BeginMenu("Export")) {
        if (ImGui::MenuItem("CSV")) {
            TableExporter::exportTables(provider, selectedNodeTables, ExportFormat::CSV, dbType);
        }
        if (ImGui::MenuItem("JSON")) {
            TableExporter::exportTables(provider, selectedNodeTables, ExportFormat::JSON, dbType);
        }
        if (ImGui::MenuItem("SQL")) {
            TableExporter::exportTables(provider, selectedNodeTables, ExportFormat::SQL, dbType);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(DELETE_LABEL)) {
        const std::vector<std::string> names = std::move(selectedNames);
        const size_t count = names.size();
        Alert::show("Delete Tables",
                    std::format("Permanently delete {} table{}? This is irreversible.", count,
                                count == 1 ? "" : "s"),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Delete",
                      [names, dropOne]() {
                          for (const auto& n : names) {
                              dropOne(n);
                          }
                      },
                      AlertButton::Style::Destructive}});
    }
}

bool DatabaseHierarchy::renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                                               const std::string& icon, const ImU32 iconColor,
                                               const ImGuiTreeNodeFlags flags) {
    const std::string fullLabel = std::format("   {}###{}", label, nodeId);

    ImGui::PushID(nodeId.c_str());
    const ImGuiID id = ImGui::GetID("hover");
    ImGui::PopID();

    const float dt = ImGui::GetIO().DeltaTime;

    // check if this item will be hovered (predict based on cursor position)
    const ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    const float itemHeight = ImGui::GetFrameHeight();
    const float itemWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 itemMin = cursorPos;
    const ImVec2 itemMax = ImVec2(cursorPos.x + itemWidth, cursorPos.y + itemHeight);
    const bool anyPopupOpen =
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool willBeHovered = !anyPopupOpen && ImGui::IsMouseHoveringRect(itemMin, itemMax);

    if (willBeHovered) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImVec4 hoverColor = colors.surface2;
        hoverColor.w = 0.8f;
        ImGui::GetWindowDrawList()->AddRectFilled(itemMin, itemMax,
                                                  ImGui::ColorConvertFloat4ToU32(hoverColor), 0);
    }

    const bool isOpen = ImGui::TreeNodeEx(fullLabel.c_str(), flags);

    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, iconColor, icon.c_str());

    return isOpen;
}

void DatabaseHierarchy::renderRootNode() {
    if (!db) {
        return;
    }

    checkPostgresToolStatus();

    prevVisibleTables_ = std::move(currVisibleTables_);
    currVisibleTables_.clear();

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const auto dbType = db->getConnectionInfo().type;

    if (dbType == DatabaseType::SQLITE) {
        renderSQLiteNode();
    } else if (dbType == DatabaseType::POSTGRESQL || dbType == DatabaseType::REDSHIFT) {
        auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get());
        if (!pgDb) {
            return;
        }

        if (!pgDb->areDatabasesLoaded() && !pgDb->isLoadingDatabases()) {
            pgDb->refreshDatabaseNames();
        }

        if (pgDb->isLoadingDatabases()) {
            pgDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (pgDb->areDatabasesLoaded()) {
            // check deferred sql editor open
            if (!pendingEditorOpenDbName_.empty()) {
                auto* pendingDb = pgDb->getDatabaseData(pendingEditorOpenDbName_);
                if (!pendingDb) {
                    // database was deleted while waiting
                    pendingEditorOpenDbName_.clear();
                } else {
                    // Open directly at database level (no need to wait for schemas)
                    app.getTabManager()->createSQLEditorTab("", pendingDb);
                    pendingEditorOpenDbName_.clear();
                }
            }

            const auto& databases = pgDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr && !hiddenDatabases_.contains(dbDataPtr->name)) {
                    renderPostgresDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::MYSQL || dbType == DatabaseType::MARIADB) {
        auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get());
        if (!mysqlDb) {
            return;
        }

        // Multi-database mode
        if (!mysqlDb->areDatabasesLoaded() && !mysqlDb->isLoadingDatabases()) {
            mysqlDb->refreshDatabaseNames();
        }

        if (mysqlDb->isLoadingDatabases()) {
            mysqlDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (mysqlDb->areDatabasesLoaded()) {
            const auto& databases = mysqlDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                // empty name = schema-less server pool node, not a database
                if (dbDataPtr && !dbDataPtr->name.empty() &&
                    !hiddenDatabases_.contains(dbDataPtr->name)) {
                    renderMySQLDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::MONGODB) {
        auto* mongoDb = dynamic_cast<MongoDBDatabase*>(db.get());
        if (!mongoDb) {
            return;
        }

        if (!mongoDb->areDatabasesLoaded() && !mongoDb->isLoadingDatabases()) {
            mongoDb->refreshDatabaseNames();
        }

        if (mongoDb->isLoadingDatabases()) {
            mongoDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (mongoDb->areDatabasesLoaded()) {
            const auto& databases = mongoDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr && !hiddenDatabases_.contains(dbDataPtr->name)) {
                    renderMongoDBDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::MSSQL) {
        auto* mssqlDb = dynamic_cast<MSSQLDatabase*>(db.get());
        if (!mssqlDb) {
            return;
        }

        if (!mssqlDb->areDatabasesLoaded() && !mssqlDb->isLoadingDatabases()) {
            mssqlDb->refreshDatabaseNames();
        }

        if (mssqlDb->isLoadingDatabases()) {
            mssqlDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (mssqlDb->areDatabasesLoaded()) {
            const auto& databases = mssqlDb->getDatabaseDataMap() | std::views::values;
            for (const auto& dbDataPtr : databases) {
                if (dbDataPtr && !hiddenDatabases_.contains(dbDataPtr->name)) {
                    renderMSSQLDatabaseNode(dbDataPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::CASSANDRA) {
        auto* cassDb = dynamic_cast<CassandraDatabase*>(db.get());
        if (!cassDb) {
            return;
        }

        if (!cassDb->areDatabasesLoaded() && !cassDb->isLoadingDatabases()) {
            cassDb->refreshDatabaseNames();
        }

        if (cassDb->isLoadingDatabases()) {
            cassDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_keyspaces_spinner", 6.0f, 2,
                             ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (cassDb->areDatabasesLoaded()) {
            const auto& keyspaces = cassDb->getDatabaseDataMap() | std::views::values;
            for (const auto& ksPtr : keyspaces) {
                if (ksPtr && !hiddenDatabases_.contains(ksPtr->name)) {
                    renderCassandraDatabaseNode(ksPtr.get());
                }
            }
        }
    } else if (dbType == DatabaseType::ORACLE) {
        auto* oracleDb = dynamic_cast<OracleDatabase*>(db.get());
        if (!oracleDb) {
            return;
        }

        if (!oracleDb->areDatabasesLoaded() && !oracleDb->isLoadingDatabases()) {
            oracleDb->refreshDatabaseNames();
        }

        if (oracleDb->isLoadingDatabases()) {
            oracleDb->checkDatabasesStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_schemas_spinner", 6.0f, 2,
                             ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (oracleDb->areDatabasesLoaded()) {
            const auto& schemas = oracleDb->getDatabaseDataMap() | std::views::values;
            for (const auto& schemaPtr : schemas) {
                if (schemaPtr && !hiddenDatabases_.contains(schemaPtr->name)) {
                    renderOracleDatabaseNode(schemaPtr.get());
                }
            }
        }
    }

    // queries node — shown for all non-Redis connection types
    if (dbType != DatabaseType::REDIS) {
        renderQueriesNode();
    }

    if (dbType == DatabaseType::REDIS) {
        const auto redisDb = std::dynamic_pointer_cast<RedisDatabase>(db);
        if (!redisDb) {
            return;
        }

        // Show connection status
        if (!redisDb->isConnected()) {
            if (redisDb->isConnecting()) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                                   ICON_FA_SPINNER " Connecting...");
            } else if (redisDb->hasAttemptedConnection()) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                   ICON_FA_CIRCLE_EXCLAMATION " Connection failed");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", redisDb->getLastConnectionError().c_str());
                }
            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                   ICON_FA_DATABASE " Not connected");
            }
            return;
        }

        // Commands node — opens a Redis CLI editor tab
        {
            constexpr ImGuiTreeNodeFlags cmdFlags = ImGuiTreeNodeFlags_Leaf |
                                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_FramePadding;
            const std::string cmdId =
                std::format("redis_commands_{:p}", static_cast<const void*>(redisDb.get()));
            const std::string cmdLabel = std::format("   Commands###{}", cmdId);
            ImGui::TreeNodeEx(cmdLabel.c_str(), cmdFlags);

            const auto iconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.purple),
                                                ICON_FA_TERMINAL);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                Application::getInstance().getTabManager()->createRedisCommandEditorTab(
                    redisDb.get());
            }

            if (ImGui::BeginPopupContextItem(cmdId.c_str())) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem("Open Command Editor")) {
                    Application::getInstance().getTabManager()->createRedisCommandEditorTab(
                        redisDb.get());
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
        }

        // Pub/Sub node
        {
            constexpr ImGuiTreeNodeFlags pubsubFlags = ImGuiTreeNodeFlags_Leaf |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                       ImGuiTreeNodeFlags_FramePadding;
            const std::string pubsubId =
                std::format("redis_pubsub_{:p}", static_cast<const void*>(redisDb.get()));
            const std::string pubsubLabel = std::format("   Pub/Sub###{}", pubsubId);
            ImGui::TreeNodeEx(pubsubLabel.c_str(), pubsubFlags);

            const auto pubsubIconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(pubsubIconPos, ImGui::GetColorU32(colors.green),
                                                ICON_FA_TOWER_BROADCAST);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                Application::getInstance().getTabManager()->createRedisPubSubTab(redisDb.get());
            }

            if (ImGui::BeginPopupContextItem(pubsubId.c_str())) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem("Open Pub/Sub")) {
                    Application::getInstance().getTabManager()->createRedisPubSubTab(redisDb.get());
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
        }

        // Load database info if not loaded yet
        if (!redisDb->isDbInfoLoaded() && !redisDb->isLoadingDbInfo()) {
            redisDb->startDbInfoLoadAsync();
        }

        // Check async db info loading status
        if (redisDb->isLoadingDbInfo()) {
            redisDb->checkDbInfoStatusAsync();
        }

        // Show loading indicator if loading db info
        if (redisDb->isLoadingDbInfo()) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::Text("  Loading databases...");
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_redis_dbs", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
            return;
        }

        // Render database nodes (db0-db15)
        if (redisDb->isDbInfoLoaded()) {
            const auto& dbInfoList = redisDb->getDatabaseInfoList();
            for (const auto& dbInfo : dbInfoList) {
                const std::string dbName = std::format("db{}", dbInfo.index);

                // Skip hidden databases
                if (hiddenDatabases_.contains(dbName))
                    continue;

                const std::string dbNodeId = std::format("redis_db_{}_{:p}", dbInfo.index,
                                                         static_cast<const void*>(redisDb.get()));

                // Build label with key count
                std::string dbLabel;
                if (dbInfo.hasKeys) {
                    dbLabel = std::format("   {} ({} keys)###{}", dbName, dbInfo.keys, dbNodeId);
                } else {
                    dbLabel = std::format("   {}###{}", dbName, dbNodeId);
                }

                constexpr ImGuiTreeNodeFlags dbNodeFlags = ImGuiTreeNodeFlags_Leaf |
                                                           ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                           ImGuiTreeNodeFlags_FramePadding;

                ImGui::TreeNodeEx(dbLabel.c_str(), dbNodeFlags);

                // Database icon
                const auto dbIconPos =
                    ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                           ImGui::GetItemRectMin().y +
                               (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
                ImGui::GetWindowDrawList()->AddText(
                    dbIconPos, ImGui::GetColorU32(dbInfo.hasKeys ? colors.blue : colors.overlay1),
                    ICON_FA_DATABASE);

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    Application::getInstance().getTabManager()->createRedisKeyViewerTab(
                        redisDb.get(), "*", dbInfo.index);
                }

                // Context menu for database node
                if (ImGui::BeginPopupContextItem(dbNodeId.c_str())) {
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                        ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                    if (ImGui::MenuItem("Browse Keys")) {
                        Application::getInstance().getTabManager()->createRedisKeyViewerTab(
                            redisDb.get(), "*", dbInfo.index);
                    }
                    if (ImGui::MenuItem("Refresh")) {
                        redisDb->startDbInfoLoadAsync(true);
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndPopup();
                }
            }
        }
    }
}

void DatabaseHierarchy::renderSQLiteNode() {
    auto* sqliteDb = dynamic_cast<SQLiteDatabase*>(db.get());
    if (!sqliteDb) {
        return;
    }

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    // Render Tables section
    {
        const std::string tablesNodeId =
            std::format("sqlite_tables_{:p}", static_cast<void*>(sqliteDb));
        const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                       ImGui::GetColorU32(colors.green));

        // Context menu for Tables node
        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                app.getTabManager()->createTableEditorTab(sqliteDb);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                sqliteDb->startTablesLoadAsync();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (tablesOpen) {
            if (!sqliteDb->areTablesLoaded() && !sqliteDb->isLoadingTables()) {
                sqliteDb->startTablesLoadAsync();
            }

            if (sqliteDb->isLoadingTables()) {
                sqliteDb->checkLoadingStatus();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::TextUnformatted(LOADING_LABEL);
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (sqliteDb->tablesLoaded) {
                auto& tables = const_cast<std::vector<Table>&>(
                    const_cast<const SQLiteDatabase*>(sqliteDb)->getTables());
                if (tables.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No tables");
                    ImGui::PopStyleColor();
                } else {
                    for (auto& table : tables) {
                        renderSQLiteTableNode(table, sqliteDb);
                    }
                }
            }
            ImGui::TreePop();
        }
    }

    // Render Views section
    {
        const std::string viewsNodeId =
            std::format("sqlite_views_{:p}", static_cast<void*>(sqliteDb));
        const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                      ImGui::GetColorU32(colors.teal));

        // Context menu for Views node
        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                sqliteDb->startViewsLoadAsync();
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (viewsOpen) {
            if (!sqliteDb->viewsLoaded && !sqliteDb->isLoadingViews()) {
                sqliteDb->startViewsLoadAsync();
            }

            if (sqliteDb->isLoadingViews()) {
                sqliteDb->checkLoadingStatus();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::TextUnformatted(LOADING_LABEL);
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else {
                auto& views = const_cast<std::vector<Table>&>(
                    const_cast<const SQLiteDatabase*>(sqliteDb)->getViews());
                if (views.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No views");
                    ImGui::PopStyleColor();
                } else {
                    for (auto& view : views) {
                        renderSQLiteViewNode(view, sqliteDb);
                    }
                }
            }
            ImGui::TreePop();
        }
    }

    // Render Sequences section (sqlite_sequence — populated by INTEGER PRIMARY KEY AUTOINCREMENT)
    {
        const std::string seqNodeId =
            std::format("sqlite_sequences_{:p}", static_cast<void*>(sqliteDb));
        const bool seqOpen = renderTreeNodeWithIcon(
            "Sequences", seqNodeId, ICON_FK_SORT_NUMERIC_ASC, ImGui::GetColorU32(colors.purple));

        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                sqliteDb->startSequencesLoadAsync(true);
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (seqOpen) {
            if (!sqliteDb->sequencesLoaded && !sqliteDb->sequencesLoader.isRunning()) {
                sqliteDb->startSequencesLoadAsync();
            }

            if (sqliteDb->sequencesLoader.isRunning()) {
                sqliteDb->checkSequencesStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::TextUnformatted(LOADING_LABEL);
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_sqlite_sequences", 6.0f, 2,
                                 ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (sqliteDb->sequencesLoaded) {
                const auto& sequences = sqliteDb->getSequences();
                if (sequences.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No sequences");
                    ImGui::PopStyleColor();
                } else {
                    constexpr ImGuiTreeNodeFlags seqFlags = ImGuiTreeNodeFlags_Leaf |
                                                            ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                            ImGuiTreeNodeFlags_FramePadding;
                    for (const auto& seq : sequences) {
                        const std::string seqItemId =
                            std::format("sqlite_seq_{}_{:p}", seq, static_cast<const void*>(&seq));
                        const std::string seqLabel = std::format("   {}###{}", seq, seqItemId);
                        ImGui::TreeNodeEx(seqLabel.c_str(), seqFlags);

                        const auto iconPos = ImVec2(
                            ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                            ImGui::GetItemRectMin().y +
                                (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
                        ImGui::GetWindowDrawList()->AddText(
                            iconPos, ImGui::GetColorU32(colors.purple), ICON_FK_SORT_NUMERIC_ASC);

                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            app.getTabManager()->createSQLiteSequenceViewerTab(sqliteDb, seq);
                        }
                    }
                }
            }
            ImGui::TreePop();
        }
    }
}

void DatabaseHierarchy::renderPostgresDatabaseNode(PostgresDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.blue));
    const ImVec2 pgNodeMin = ImGui::GetItemRectMin();
    const ImVec2 pgNodeMax = ImGui::GetItemRectMax();
    if (dbData->schemasLoaded && !dbData->schemas.empty()) {
        std::vector<std::string> schemaNames;
        schemaNames.reserve(dbData->schemas.size());
        for (const auto& s : dbData->schemas)
            if (s)
                schemaNames.push_back(s->name);
        renderSchemaFilterBadge(dbData->name, schemaNames, pgNodeMin, pgNodeMax,
                                static_cast<const void*>(dbData));
    }

    // Handle expand/collapse
    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            // PostgresDatabaseNode now implements IDatabaseNode — pass directly
            // for database-level SQL editor (no SET search_path, cross-schema queries)
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startSchemasLoadAsync(true, true);
        }
        renderPostgresBackupRestoreMenus(dbData);
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = dbData->name;
            InputDialog::show(
                "Rename Database", "New name:", oldName, "Rename",
                [this, oldName](const std::string& newName) -> std::string {
                    auto [success, error] = db->renameDatabase(oldName, newName);
                    if (success) {
                        if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                            pgDb->refreshDatabaseNames();
                        }
                        return "";
                    }
                    return error;
                },
                nullptr,
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete", [this, dbName]() { pendingDropDatabase_ = dbName; },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // PostgreSQL: render schemas
        if (!dbData->schemasLoaded && !dbData->schemasLoader.isRunning()) {
            dbData->startSchemasLoadAsync();
        }

        if (dbData->schemasLoader.isRunning()) {
            dbData->checkSchemasStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_schemas", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (dbData->schemasLoaded) {
            // Render each schema
            for (auto& schema : dbData->schemas) {
                if (schema && isSchemaHidden(dbData->name, schema->name))
                    continue;
                renderPostgresSchemaNode(dbData, schema.get());
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::processPendingDatabaseDrop() {
    if (!pendingDropDatabase_) {
        return;
    }

    const std::string dbName = *pendingDropDatabase_;
    pendingDropDatabase_.reset();

    const char* noun =
        db->getConnectionInfo().type == DatabaseType::CASSANDRA ? "keyspace" : "database";

    auto [success, error] = db->dropDatabase(dbName);
    if (!success) {
        spdlog::error("Failed to drop {}: {}", noun, error);
        Alert::show("Error", std::format("Failed to drop {}: {}", noun, error));
        return;
    }

    spdlog::debug("Dropped {} '{}'", noun, dbName);
    setDatabaseHidden(dbName, false);
    db->refreshDatabaseNames();
}

void DatabaseHierarchy::processDumpOperations() {
    renderImportPreview();
    checkImportStatus();
    renderImportProgress();
    checkExportStatus();
    renderExportProgress();
}

void DatabaseHierarchy::startSqlDumpImport(MySQLDatabaseNode* dbData) {
    if (!dbData || importOp_.isRunning()) {
        return;
    }

    const std::string path = DatabaseImporter::promptForSqlDump();
    if (path.empty()) {
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Alert::show("Import Failed", std::format("Could not open '{}'.", path));
        return;
    }

    std::string buffer(static_cast<std::size_t>(IMPORT_PREVIEW_BYTES), '\0');
    in.read(buffer.data(), IMPORT_PREVIEW_BYTES);
    importPreviewTruncated_ = in.gcount() == IMPORT_PREVIEW_BYTES;
    buffer.resize(static_cast<std::size_t>(in.gcount()));

    // An extended INSERT puts a whole table on one line -- half a megabyte is
    // ordinary. Shorten each line rather than letting one of them spend the
    // budget: the structure is what identifies the dump, the row data is not.
    std::string head;
    importPreviewLines_ = 0;
    importPreviewElided_ = false;

    std::size_t pos = 0;
    while (pos < buffer.size() && importPreviewLines_ < IMPORT_PREVIEW_LINES) {
        const std::size_t eol = buffer.find('\n', pos);
        const std::size_t end = eol == std::string::npos ? buffer.size() : eol;
        const std::size_t length = end - pos;

        if (length > IMPORT_PREVIEW_LINE_CHARS) {
            head.append(buffer, pos, IMPORT_PREVIEW_LINE_CHARS);
            head += std::format(" [... {} more characters]", length - IMPORT_PREVIEW_LINE_CHARS);
            importPreviewElided_ = true;
        } else {
            head.append(buffer, pos, length);
        }
        head += '\n';
        ++importPreviewLines_;

        if (eol == std::string::npos) {
            pos = buffer.size();
            break;
        }
        pos = eol + 1;
    }

    if (pos < buffer.size()) {
        importPreviewTruncated_ = true;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    importPreviewSize_ = ec ? 0 : size;
    importPreviewPath_ = path;
    importPreviewDbName_ = dbData->name;

    importPreviewEditor_.SetPalette(
        dearsql::TextEditor::FromTheme(Application::getInstance().getCurrentColors()));
    // Not SQL: the vendored grammar cannot parse mysqldump's conditional comments
    // (/*!40101 ... */), errors on line 2 of a real dump and colors the rest in
    // patches. Uniformly plain reads better than unevenly wrong, and it skips the
    // parse entirely.
    importPreviewEditor_.SetLanguage(dearsql::TextEditor::Language::PlainText);
    importPreviewEditor_.SetShowLineNumbers(true);
    importPreviewEditor_.SetText(head);
    importPreviewEditor_.SetReadOnly(true);
    openImportPreview_ = true;
}

void DatabaseHierarchy::renderImportPreview() {
    if (!db) {
        return;
    }

    // The id carries the connection: each one has its own DatabaseHierarchy, and a
    // bare string would let two of them drive the same popup.
    const std::string popupId =
        std::format("Import SQL Dump###import_preview_{}", db->getConnectionId());

    if (openImportPreview_) {
        ImGui::OpenPopup(popupId.c_str());
        openImportPreview_ = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::TextUnformatted(std::format("Into '{}'", importPreviewDbName_).c_str());

    const std::string fileLabel =
        std::format("{}  ·  {}", std::filesystem::path(importPreviewPath_).filename().string(),
                    formatBytes(importPreviewSize_));
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(fileLabel.c_str()).x);
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext1);
    ImGui::TextUnformatted(fileLabel.c_str());
    std::string extent =
        importPreviewTruncated_
            ? std::format("First {} lines — head of the file only", importPreviewLines_)
            : std::format("Whole file — {} lines", importPreviewLines_);
    if (importPreviewElided_) {
        extent += " · long rows shortened";
    }
    ImGui::TextUnformatted(extent.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // The palette is applied once at load: SetPalette marks the whole buffer for
    // rehighlighting, and doing that per frame over 100,000 characters is ruinous.
    // Nothing can change the theme meanwhile -- this is a modal.
    const float buttonsHeight =
        ImGui::GetFrameHeightWithSpacing() + Theme::Spacing::M * 2.0f + Theme::Spacing::S;
    importPreviewEditor_.Render("##import_preview_sql",
                                ImVec2(0, ImGui::GetContentRegionAvail().y - buttonsHeight), true);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    constexpr float buttonWidth = 120.0f;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttonWidth * 2.0f - Theme::Spacing::M);
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, Theme::Spacing::M);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colors.red.x, colors.red.y, colors.red.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(colors.red.x, colors.red.y, colors.red.z, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(colors.red.x * 0.8f, colors.red.y * 0.8f,
                                                        colors.red.z * 0.8f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
    if (ImGui::Button("Import", ImVec2(buttonWidth, 0.0f))) {
        beginSqlDumpImport();
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(4);

    ImGui::EndPopup();
}

void DatabaseHierarchy::beginSqlDumpImport() {
    auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get());
    if (!mysqlDb || importOp_.isRunning()) {
        return;
    }

    // Re-resolve instead of carrying the node across the confirmation, and read
    // through the const accessor so a missing entry cannot create a phantom one.
    const MySQLDatabase& constDb = *mysqlDb;
    const auto& cache = constDb.getDatabaseDataMap();
    const auto it = cache.find(importPreviewDbName_);
    if (it == cache.end() || !it->second) {
        Alert::show("Import Failed",
                    std::format("Database '{}' is no longer available.", importPreviewDbName_));
        return;
    }

    MySQLDatabaseNode* dbData = it->second.get();
    importProgress_ = std::make_shared<DatabaseImporter::Progress>();
    importDbName_ = importPreviewDbName_;
    importStartTime_ = ImGui::GetTime();

    importOp_.startCancellable([dbData, path = importPreviewPath_,
                                progress = importProgress_](const std::stop_token& stopToken) {
        return DatabaseImporter::runSqlDump(dbData, path, *progress, stopToken);
    });
}

void DatabaseHierarchy::startSqlDumpExport(MySQLDatabaseNode* dbData) {
    if (!dbData || exportOp_.isRunning()) {
        return;
    }

    const std::string path = DatabaseExporter::promptForSqlDumpDestination(
        std::format("{}_{}.sql", sanitizeBackupFileName(dbData->name), timestampForFileName()));
    if (path.empty()) {
        return;
    }

    exportProgress_ = std::make_shared<DatabaseExporter::Progress>();
    exportDbName_ = dbData->name;

    exportOp_.startCancellable(
        [dbData, path, progress = exportProgress_](const std::stop_token& stopToken) {
            return DatabaseExporter::runSqlDump(dbData, path, *progress, stopToken);
        });
}

void DatabaseHierarchy::checkExportStatus() {
    exportOp_.check([this](const DatabaseExporter::Result result) {
        if (result.success) {
            Alert::show("Export Complete", std::format("Wrote {} object(s) and {} row(s) to '{}'.",
                                                       result.objects, result.rows, result.path));
        } else if (result.cancelled) {
            Alert::show("Export Cancelled",
                        std::format("Stopped after {} object(s). '{}' is incomplete.",
                                    result.objects, result.path));
        } else {
            Alert::show("Export Failed", std::format("The export failed after {} object(s):\n\n{}",
                                                     result.objects, result.error));
        }
        exportProgress_.reset();
    });
}

void DatabaseHierarchy::renderExportProgress() {
    if (!exportOp_.isRunning() || !exportProgress_) {
        return;
    }

    const int done = exportProgress_->objectsDone.load(std::memory_order_relaxed);
    const int total = exportProgress_->objectsTotal.load(std::memory_order_relaxed);
    const auto rows = exportProgress_->rowsWritten.load(std::memory_order_relaxed);
    const auto bytes = exportProgress_->bytesWritten.load(std::memory_order_relaxed);
    const bool cancelling = exportProgress_->cancelRequested.load(std::memory_order_relaxed);

    const float fraction = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;

    if (renderProgressPanel("##sql_export_progress",
                            cancelling ? "Cancelling export..." : "Exporting SQL dump", fraction,
                            std::format("{} / {} objects", done, total),
                            std::format("{} rows written  ·  {}", rows, formatBytes(bytes)),
                            "999999999 rows written  ·  999.99 GB", cancelling)) {
        exportProgress_->cancelRequested.store(true, std::memory_order_relaxed);
    }
}

void DatabaseHierarchy::checkImportStatus() {
    importOp_.check([this](const DatabaseImporter::Result result) {
        if (result.success) {
            Alert::show("Import Complete", std::format("Applied {} statement(s) from '{}'.",
                                                       result.applied, result.path));
        } else if (result.cancelled) {
            Alert::show("Import Cancelled",
                        std::format("Stopped after {} statement(s). Changes already applied were "
                                    "not rolled back.",
                                    result.applied));
        } else {
            // Statements are sent to the server in batches, so the exact failing
            // statement is not known -- only that it followed the last batch that
            // succeeded. Do not name a statement number that could be wrong.
            Alert::show("Import Failed",
                        std::format("The import failed after {} statement(s):\n\n{}\n\nWhat had "
                                    "already been applied was not rolled back.",
                                    result.applied, result.error));
        }

        if (result.applied > 0) {
            if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
                if (auto* dbData = mysqlDb->getDatabaseData(importDbName_)) {
                    dbData->startTablesLoadAsync(true);
                    dbData->startViewsLoadAsync(true);
                }
            }
        }

        importProgress_.reset();
        importDbName_.clear();
    });
}

void DatabaseHierarchy::renderImportProgress() {
    if (!importOp_.isRunning() || !importProgress_) {
        return;
    }

    const auto total = importProgress_->totalBytes.load(std::memory_order_relaxed);
    const auto read = importProgress_->bytesRead.load(std::memory_order_relaxed);
    const int applied = importProgress_->statementsApplied.load(std::memory_order_relaxed);
    const bool cancelling = importProgress_->cancelRequested.load(std::memory_order_relaxed);

    // file_size() can fail; an indeterminate bar is honest, a fraction of zero is not.
    const float fraction =
        total > 0 ? static_cast<float>(static_cast<double>(read) / static_cast<double>(total))
                  : -1.0f * static_cast<float>(ImGui::GetTime());
    const std::string overlay = total > 0
                                    ? std::format("{} / {}", formatBytes(read), formatBytes(total))
                                    : formatBytes(read);

    const double elapsed = ImGui::GetTime() - importStartTime_;
    std::string statusText = std::format("{} statements applied", applied);
    if (elapsed > 0.5) {
        const double bytesPerSecond = static_cast<double>(read) / elapsed;
        statusText += std::format("  ·  {}/s  ·  {:.0f} stmt/s",
                                  formatBytes(static_cast<std::uint64_t>(bytesPerSecond)),
                                  static_cast<double>(applied) / elapsed);
        if (total > read && bytesPerSecond > 0.0) {
            statusText += std::format(
                "  ·  {} left", formatDuration(static_cast<double>(total - read) / bytesPerSecond));
        }
    }

    if (renderProgressPanel(
            "##sql_import_progress", cancelling ? "Cancelling import..." : "Importing SQL dump",
            fraction, overlay, statusText,
            "999999 statements applied  ·  999.9 MB/s  ·  999999 stmt/s  ·  99h 59m left",
            cancelling)) {
        importProgress_->cancelRequested.store(true, std::memory_order_relaxed);
    }
}

void DatabaseHierarchy::checkPostgresToolStatus() {
    postgresToolOp_.check([this](const PostgresToolResult result) {
        const std::string title =
            postgresToolTitle_.empty() ? "PostgreSQL Operation" : postgresToolTitle_;
        if (result.success) {
            Alert::show(title, result.message);
            if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                if (!postgresToolRefreshDbName_.empty()) {
                    if (auto* dbData = pgDb->getDatabaseData(postgresToolRefreshDbName_)) {
                        dbData->startSchemasLoadAsync(true, true);
                    }
                }
                if (postgresToolRefreshDatabaseList_ || !postgresToolRefreshDbName_.empty()) {
                    pgDb->refreshDatabaseNames();
                }
            }
        } else {
            Alert::show(title,
                        std::format("{}\n\n{}", result.message, summarizeToolFailure(result)));
        }
        postgresToolTitle_.clear();
        postgresToolRefreshDbName_.clear();
        postgresToolRefreshDatabaseList_ = false;
    });
}

void DatabaseHierarchy::renderPostgresBackupRestoreMenus(PostgresDatabaseNode* dbData) {
    if (!dbData || !dbData->parentDb) {
        return;
    }

    if (dbData->parentDb->getConnectionInfo().type == DatabaseType::REDSHIFT) {
        return;
    }

    const bool toolBusy = postgresToolOp_.isRunning();
    if (ImGui::BeginMenu(BACKUP_LABEL, !toolBusy)) {
        if (ImGui::MenuItem("Custom Archive (.dump)")) {
            startPostgresBackup(dbData, PostgresBackupFormat::Custom, false, false);
        }
        if (ImGui::MenuItem("Custom Archive + CREATE DATABASE (.dump)")) {
            startPostgresBackup(dbData, PostgresBackupFormat::Custom, true, false);
        }
        if (ImGui::MenuItem("Custom Archive, No Owner (.dump)")) {
            startPostgresBackup(dbData, PostgresBackupFormat::Custom, false, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Plain SQL (.sql)")) {
            startPostgresBackup(dbData, PostgresBackupFormat::PlainSql, false, false);
        }
        if (ImGui::MenuItem("Plain SQL + CREATE DATABASE (.sql)")) {
            startPostgresBackup(dbData, PostgresBackupFormat::PlainSql, true, false);
        }
        if (ImGui::MenuItem("Plain SQL, No Owner (.sql)")) {
            startPostgresBackup(dbData, PostgresBackupFormat::PlainSql, false, true);
        }
        ImGui::EndMenu();
    } else if (toolBusy && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A PostgreSQL backup or restore is already running");
    }

    if (ImGui::BeginMenu(RESTORE_LABEL, !toolBusy)) {
        if (ImGui::MenuItem("Into This Database")) {
            startPostgresRestore(dbData, false, false);
        }
        if (ImGui::MenuItem("Clean Then Restore")) {
            startPostgresRestore(dbData, true, false);
        }
        if (ImGui::MenuItem("Create Database From Backup")) {
            startPostgresRestore(dbData, false, true);
        }
        ImGui::EndMenu();
    } else if (toolBusy && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A PostgreSQL backup or restore is already running");
    }
}

void DatabaseHierarchy::startPostgresBackup(PostgresDatabaseNode* dbData,
                                            const PostgresBackupFormat format,
                                            const bool includeCreateDatabase, const bool noOwner) {
    if (!dbData || !dbData->parentDb || postgresToolOp_.isRunning()) {
        return;
    }

    if (!ensurePostgresToolsAvailable({"pg_dump"})) {
        return;
    }

    const std::string path =
        FileDialog::savePostgresBackupFile(defaultPostgresBackupName(dbData->name, format));
    if (path.empty()) {
        return;
    }

    auto info = dbData->parentDb->getConnectionInfo();
    info.database = dbData->name;
    PostgresBackupOptions options{info,   dbData->name,          std::filesystem::path(path),
                                  format, includeCreateDatabase, noOwner};

    postgresToolTitle_ = "Backup Database";
    postgresToolRefreshDbName_.clear();
    postgresToolRefreshDatabaseList_ = false;
    postgresToolOp_.start([options = std::move(options)]() {
        return PostgresBackupService::backupDatabase(options);
    });
}

void DatabaseHierarchy::startPostgresRestore(PostgresDatabaseNode* dbData,
                                             const bool cleanBeforeRestore,
                                             const bool createDatabase) {
    if (!dbData || !dbData->parentDb || postgresToolOp_.isRunning()) {
        return;
    }

    const std::string path = FileDialog::openPostgresBackupFile();
    if (path.empty()) {
        return;
    }

    const bool plainSqlBackup = isPlainSqlBackupPath(path);
    if (cleanBeforeRestore && plainSqlBackup) {
        Alert::show("Restore Database",
                    "Clean restore requires a custom PostgreSQL archive because pg_restore "
                    "generates the DROP statements. Plain SQL backups can still be restored "
                    "normally if they already contain the intended DDL.");
        return;
    }

    if (!ensurePostgresToolsAvailable({plainSqlBackup ? "psql" : "pg_restore"})) {
        return;
    }

    const std::string dbName = dbData->name;
    const std::string action = createDatabase ? "create a database from this backup"
                               : cleanBeforeRestore
                                   ? std::format("drop matching objects in '{}' first", dbName)
                                   : std::format("restore into '{}'", dbName);
    const std::string createNote =
        createDatabase && plainSqlBackup
            ? "\n\nPlain SQL create-database restores require a backup that contains CREATE "
              "DATABASE and connection commands."
            : "";
    Alert::show(
        "Restore Database",
        std::format("This will {}.\n\nBackup file:\n{}{}\n\nContinue?", action, path, createNote),
        {{"Cancel", nullptr, AlertButton::Style::Cancel},
         {"Restore",
          [this, dbData, dbName, path, cleanBeforeRestore, createDatabase]() {
              if (!dbData || !dbData->parentDb || postgresToolOp_.isRunning()) {
                  return;
              }

              auto info = dbData->parentDb->getConnectionInfo();
              info.database = dbName;
              PostgresRestoreOptions options{
                  info, dbName,        std::filesystem::path(path), cleanBeforeRestore,
                  true, createDatabase};

              postgresToolTitle_ = "Restore Database";
              postgresToolRefreshDbName_ = createDatabase ? "" : dbName;
              postgresToolRefreshDatabaseList_ = createDatabase;
              postgresToolOp_.start([options = std::move(options)]() {
                  return PostgresBackupService::restoreDatabase(options);
              });
          },
          AlertButton::Style::Destructive}});
}

void DatabaseHierarchy::renderPostgresSchemaNode(const PostgresDatabaseNode* dbData,
                                                 PostgresSchemaNode* schemaData) {
    if (!dbData || !schemaData) {
        return;
    }

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId =
        std::format("schema_{}_{:p}", schemaData->name, static_cast<void*>(schemaData));
    const bool isOpen = renderTreeNodeWithIcon(schemaData->name, nodeId, ICON_FK_FOLDER,
                                               ImGui::GetColorU32(colors.yellow));

    // Context menu for schema
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            // schemaData implements IDatabaseNode, so we can pass it directly
            app.getTabManager()->createSQLEditorTab("", schemaData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(schemaData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            schemaData->startTablesLoadAsync(true);
            schemaData->startViewsLoadAsync(true);
            schemaData->startMaterializedViewsLoadAsync(true);
            schemaData->startSequencesLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = schemaData->name;
            InputDialog::show(
                "Rename Schema", "New name:", oldName, "Rename",
                [schemaData](const std::string& newName) -> std::string {
                    auto [success, error] = schemaData->renameSchema(newName);
                    return success ? "" : error;
                },
                nullptr,
                [oldName](const std::string& newName) -> std::string {
                    if (newName == oldName)
                        return "New name must be different";
                    return "";
                });
        }
        if (ImGui::MenuItem(DELETE_LABEL)) {
            Alert::show(
                "Delete Schema",
                std::format("Permanently delete '{}' and ALL its contents? This is irreversible.",
                            schemaData->name),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [schemaData]() {
                      auto [success, error] = schemaData->dropSchema();
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete schema: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // Render Tables section
        {
            const std::string tablesNodeId = std::format("tables_{}_{:p}", schemaData->name,
                                                         static_cast<void*>(&schemaData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            // Context menu for Tables node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    app.getTabManager()->createTableEditorTab(schemaData, schemaData->name);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!schemaData->tablesLoaded && !schemaData->tablesLoader.isRunning()) {
                    schemaData->startTablesLoadAsync();
                }

                if (schemaData->tablesLoader.isRunning()) {
                    schemaData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->tablesLoaded) {
                    if (schemaData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : schemaData->tables) {
                            renderTableNode(table, schemaData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Views section
        {
            const std::string viewsNodeId = std::format("views_{}_{:p}", schemaData->name,
                                                        static_cast<void*>(&schemaData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            // Context menu for Views node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startViewsLoadAsync(true); // Force refresh
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!schemaData->viewsLoaded && !schemaData->viewsLoader.isRunning()) {
                    schemaData->startViewsLoadAsync();
                }

                if (schemaData->viewsLoader.isRunning()) {
                    schemaData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->viewsLoaded) {
                    if (schemaData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : schemaData->views) {
                            renderViewNode(view, schemaData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Materialized Views section
        {
            const std::string matViewsNodeId =
                std::format("matviews_{}_{:p}", schemaData->name,
                            static_cast<void*>(&schemaData->materializedViews));
            const bool matViewsOpen =
                renderTreeNodeWithIcon("Materialized Views", matViewsNodeId, ICON_FK_EYE,
                                       ImGui::GetColorU32(colors.lavender));

            // Context menu for Materialized Views node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startMaterializedViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (matViewsOpen) {
                if (!schemaData->materializedViewsLoaded &&
                    !schemaData->materializedViewsLoader.isRunning()) {
                    schemaData->startMaterializedViewsLoadAsync();
                }

                if (schemaData->materializedViewsLoader.isRunning()) {
                    schemaData->checkMaterializedViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_matviews", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->materializedViewsLoaded) {
                    if (schemaData->materializedViews.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No materialized views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& mv : schemaData->materializedViews) {
                            renderViewNode(mv, schemaData, true);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Sequences section
        {
            const std::string seqNodeId = std::format("sequences_{}_{:p}", schemaData->name,
                                                      static_cast<void*>(&schemaData->sequences));
            const bool seqOpen =
                renderTreeNodeWithIcon("Sequences", seqNodeId, ICON_FK_SORT_NUMERIC_ASC,
                                       ImGui::GetColorU32(colors.purple));

            // Context menu for Sequences node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startSequencesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (seqOpen) {
                if (!schemaData->sequencesLoaded && !schemaData->sequencesLoader.isRunning()) {
                    schemaData->startSequencesLoadAsync();
                }

                if (schemaData->sequencesLoader.isRunning()) {
                    schemaData->checkSequencesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_sequences", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->sequencesLoaded) {
                    if (schemaData->sequences.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No sequences");
                        ImGui::PopStyleColor();
                    } else {
                        constexpr ImGuiTreeNodeFlags seqFlags =
                            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                            ImGuiTreeNodeFlags_FramePadding;
                        for (const auto& seq : schemaData->sequences) {
                            const std::string seqItemId =
                                std::format("seq_{}_{:p}", seq, static_cast<const void*>(&seq));
                            const std::string seqLabel = std::format("   {}###{}", seq, seqItemId);
                            ImGui::TreeNodeEx(seqLabel.c_str(), seqFlags);

                            const auto iconPos = ImVec2(
                                ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                                ImGui::GetItemRectMin().y +
                                    (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) *
                                        0.5f);
                            ImGui::GetWindowDrawList()->AddText(iconPos,
                                                                ImGui::GetColorU32(colors.purple),
                                                                ICON_FK_SORT_NUMERIC_ASC);

                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                                Application::getInstance()
                                    .getTabManager()
                                    ->createPostgresSequenceViewerTab(schemaData, seq);
                            }
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Routines section
        {
            const std::string routineNodeId = std::format(
                "routines_{}_{:p}", schemaData->name, static_cast<void*>(&schemaData->routines));
            const bool routineOpen = renderTreeNodeWithIcon("Routines", routineNodeId, ICON_FA_CODE,
                                                            ImGui::GetColorU32(colors.yellow));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startRoutinesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (routineOpen) {
                if (!schemaData->routinesLoaded && !schemaData->routinesLoader.isRunning()) {
                    schemaData->startRoutinesLoadAsync();
                }

                if (schemaData->routinesLoader.isRunning()) {
                    schemaData->checkRoutinesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_routines", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->routinesLoaded) {
                    if (schemaData->routines.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No routines");
                        ImGui::PopStyleColor();
                    } else {
                        renderRoutineItems(schemaData->routines, schemaData);
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMySQLDatabaseNode(MySQLDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.blue));

    // Handle expand/collapse
    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        const bool importBusy = importOp_.isRunning();
        if (ImGui::BeginMenu("Import", !importBusy)) {
            if (ImGui::MenuItem("SQL Dump")) {
                startSqlDumpImport(dbData);
            }
            ImGui::EndMenu();
        } else if (importBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("An import is already running");
        }
        const bool exportBusy = exportOp_.isRunning();
        if (ImGui::BeginMenu("Export", !exportBusy)) {
            if (ImGui::MenuItem("SQL Dump")) {
                startSqlDumpExport(dbData);
            }
            ImGui::EndMenu();
        } else if (exportBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("An export is already running");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(RENAME_LABEL)) {
            const std::string oldName = dbData->name;
            Alert::show("Rename Database",
                        "MySQL does not support direct database renaming. You need to "
                        "create a new database, copy all data, and drop the old one.");
        }
        const bool dumpBusy = hasRunningSqlDump(dbData->name);
        if (ImGui::MenuItem(DELETE_LABEL, nullptr, false, !dumpBusy)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete", [this, dbName]() { pendingDropDatabase_ = dbName; },
                  AlertButton::Style::Destructive}});
        }
        if (dumpBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Cannot delete while a SQL dump is running on this database");
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // Render Tables section
        {
            const std::string tablesNodeId =
                std::format("tables_{}_{:p}", dbData->name, static_cast<void*>(&dbData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            // Context menu for Tables node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    app.getTabManager()->createTableEditorTab(dbData);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!dbData->tablesLoaded && !dbData->tablesLoader.isRunning()) {
                    dbData->startTablesLoadAsync();
                }

                if (dbData->tablesLoader.isRunning()) {
                    dbData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->tablesLoaded) {
                    if (dbData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : dbData->tables) {
                            renderMySQLTableNode(table, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Views section
        {
            const std::string viewsNodeId =
                std::format("views_{}_{:p}", dbData->name, static_cast<void*>(&dbData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            // Context menu for Views node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!dbData->viewsLoaded && !dbData->viewsLoader.isRunning()) {
                    dbData->startViewsLoadAsync();
                }

                if (dbData->viewsLoader.isRunning()) {
                    dbData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->viewsLoaded) {
                    if (dbData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : dbData->views) {
                            renderMySQLViewNode(view, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Routines section
        {
            const std::string routineNodeId = std::format("routines_{}_{:p}", dbData->name,
                                                          static_cast<void*>(&dbData->routines));
            const bool routineOpen = renderTreeNodeWithIcon("Routines", routineNodeId, ICON_FA_CODE,
                                                            ImGui::GetColorU32(colors.yellow));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startRoutinesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (routineOpen) {
                if (!dbData->routinesLoaded && !dbData->routinesLoader.isRunning()) {
                    dbData->startRoutinesLoadAsync();
                }

                if (dbData->routinesLoader.isRunning()) {
                    dbData->checkRoutinesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_routines", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->routinesLoaded) {
                    if (dbData->routines.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No routines");
                        ImGui::PopStyleColor();
                    } else {
                        renderRoutineItems(dbData->routines, dbData);
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderTableNode(Table& table, PostgresSchemaNode* schemaNode) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&table);
    const bool isSelected = selectedTables_.count(&table) > 0;
    const ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                          ImGuiTreeNodeFlags_FramePadding |
                                          (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string tableNodeId =
        std::format("pg_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    // dim size badge anchored to the right edge of the sidebar
    if (table.sizeBytes >= 0) {
        const std::string sizeText = formatByteSize(table.sizeBytes);
        const ImVec2 textSize = ImGui::CalcTextSize(sizeText.c_str());
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const float yCenter =
            itemMin.y + (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f;
        const ImVec2 textPos = ImVec2(rightEdge - textSize.x - Theme::Spacing::M, yCenter);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(colors.subtext0),
                                            sizeText.c_str());
    }

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        handleTableClick(&table);
    }

    // Check if table is refreshing
    const bool isRefreshing = schemaNode->isTableRefreshing(table.name);

    // Show loading indicator if refreshing
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        schemaNode->checkTableRefreshStatusAsync(table.name);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaNode, table);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool isMultiSelect = selectedTables_.size() > 1 && selectedTables_.count(&table) > 0;
        if (isMultiSelect) {
            renderMultiSelectMenuContent(
                schemaNode, schemaNode->getTables(),
                [schemaNode](const std::string& n) { schemaNode->dropTable(n); },
                schemaNode->getDatabaseType());
        } else {
            if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
                app.getTabManager()->createTableViewerTab(schemaNode, table);
            }
            if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
                app.getTabManager()->createTableEditorTab(schemaNode, table, schemaNode->name);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                schemaNode->startTableRefreshAsync(table.name);
            }
            TableExporter::renderExportMenu(schemaNode, table, schemaNode->getDatabaseType());
            TableImporter::renderImportMenu(schemaNode, table.name);
            ImGui::Separator();
            if (ImGui::MenuItem(RENAME_LABEL)) {
                const std::string oldName = table.name;
                InputDialog::show(
                    "Rename Table", "New name:", oldName, "Rename",
                    [schemaNode, oldName](const std::string& newName) -> std::string {
                        auto [success, error] = schemaNode->renameTable(oldName, newName);
                        return success ? "" : error;
                    },
                    nullptr,
                    [oldName](const std::string& newName) -> std::string {
                        if (newName == oldName)
                            return "New name must be different";
                        return "";
                    });
            }
            if (ImGui::MenuItem(TRUNCATE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show("Truncate Table",
                            std::format("Remove all rows from '{}.{}'? This is irreversible.",
                                        schemaNode->name, tableName),
                            {{"Cancel", nullptr, AlertButton::Style::Cancel},
                             {"Truncate",
                              [schemaNode, tableName]() {
                                  auto [success, error] = schemaNode->truncateTable(tableName);
                                  if (!success) {
                                      Alert::show(
                                          "Error",
                                          std::format("Failed to truncate table: {}", error));
                                  }
                              },
                              AlertButton::Style::Destructive}});
            }
            if (ImGui::MenuItem(DELETE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show("Delete Table",
                            std::format("Permanently delete '{}.{}'? This is irreversible.",
                                        schemaNode->name, tableName),
                            {{"Cancel", nullptr, AlertButton::Style::Cancel},
                             {"Delete",
                              [schemaNode, tableName]() {
                                  auto [success, error] = schemaNode->dropTable(tableName);
                                  if (!success) {
                                      Alert::show("Error",
                                                  std::format("Failed to delete table: {}", error));
                                  }
                              },
                              AlertButton::Style::Destructive}});
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // Columns section (PostgreSQL)
        {
            const std::string columnsNodeId =
                std::format("pg_columns_{}_{:p}", table.name, static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (column.isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }

                    const std::string columnNodeId =
                        std::format("pg_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    // Context menu for column
                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}.{}'?", schemaNode->name,
                                            tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [schemaNode, tblName, colName]() {
                                      auto [success, error] =
                                          schemaNode->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("pg_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                            fk.targetTable, fk.targetColumn);
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                        if (ImGui::IsItemHovered() && !fk.name.empty()) {
                            ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No foreign keys");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // References section (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                    if (ImGui::IsItemHovered()) {
                        std::string tooltip =
                            std::format("{}.{} -> {}.{}", ref.targetTable, ref.sourceColumn,
                                        table.name, ref.targetColumn);
                        if (!ref.name.empty()) {
                            tooltip += std::format("\nConstraint: {}", ref.name);
                        }
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderViewNode(Table& view, PostgresSchemaNode* schemaData,
                                       bool isMaterializedView) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    // Draw icon
    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaData, view);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(schemaData, view);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string viewName = view.name;
            const std::string typeLabel = isMaterializedView ? "Materialized View" : "View";
            Alert::show(
                std::format("Delete {}", typeLabel),
                std::format("Permanently delete {} '{}.{}'? This is irreversible.", typeLabel,
                            schemaData->name, viewName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete",
                  [schemaData, viewName, isMaterializedView]() {
                      auto [success, error] = schemaData->dropView(viewName, isMaterializedView);
                      if (!success) {
                          Alert::show("Error", std::format("Failed to delete view: {}", error));
                      }
                  },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMySQLTableNode(Table& table, MySQLDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&table);
    const bool isSelected = selectedTables_.count(&table) > 0;
    const ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                          ImGuiTreeNodeFlags_FramePadding |
                                          (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string tableNodeId =
        std::format("mysql_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        handleTableClick(&table);
    }

    // Check if table is refreshing
    const bool isRefreshing = dbData->isTableRefreshing(table.name);

    // Show loading indicator if refreshing
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        dbData->checkTableRefreshStatusAsync(table.name);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, table);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool isMultiSelect = selectedTables_.size() > 1 && selectedTables_.count(&table) > 0;
        if (isMultiSelect) {
            renderMultiSelectMenuContent(
                dbData, dbData->getTables(),
                [dbData](const std::string& n) { dbData->dropTable(n); },
                dbData->getDatabaseType());
        } else {
            if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
                app.getTabManager()->createTableViewerTab(dbData, table);
            }
            if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
                app.getTabManager()->createTableEditorTab(dbData, table);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                dbData->startTableRefreshAsync(table.name);
            }
            TableExporter::renderExportMenu(dbData, table, dbData->getDatabaseType());
            TableImporter::renderImportMenu(dbData, table.name);
            ImGui::Separator();
            if (ImGui::MenuItem(RENAME_LABEL)) {
                const std::string oldName = table.name;
                InputDialog::show(
                    "Rename Table", "New name:", oldName, "Rename",
                    [dbData, oldName](const std::string& newName) -> std::string {
                        auto [success, error] = dbData->renameTable(oldName, newName);
                        return success ? "" : error;
                    },
                    nullptr,
                    [oldName](const std::string& newName) -> std::string {
                        if (newName == oldName)
                            return "New name must be different";
                        return "";
                    });
            }
            if (ImGui::MenuItem(TRUNCATE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Truncate Table",
                    std::format("Remove all rows from '{}'? This is irreversible.", tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Truncate",
                      [dbData, tableName]() {
                          auto [success, error] = dbData->truncateTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to truncate table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
            if (ImGui::MenuItem(DELETE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Delete Table",
                    std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                                tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Delete",
                      [dbData, tableName]() {
                          auto [success, error] = dbData->dropTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to delete table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // Columns section (MySQL)
        {
            const std::string columnsNodeId = std::format("mysql_columns_{}_{:p}", table.name,
                                                          static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (column.isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }

                    const std::string columnNodeId =
                        std::format("mysql_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    // Context menu for column (MySQL)
                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [dbData, tblName, colName]() {
                                      auto [success, error] = dbData->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section (MySQL)
        {
            const std::string fkNodeId =
                std::format("mysql_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                            fk.targetTable, fk.targetColumn);
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                        if (ImGui::IsItemHovered() && !fk.name.empty()) {
                            ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No foreign keys");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // References section (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                    if (ImGui::IsItemHovered()) {
                        std::string tooltip =
                            std::format("{}.{} -> {}.{}", ref.targetTable, ref.sourceColumn,
                                        table.name, ref.targetColumn);
                        if (!ref.name.empty()) {
                            tooltip += std::format("\nConstraint: {}", ref.name);
                        }
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMySQLViewNode(Table& view, MySQLDatabaseNode* dbData) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    // Draw icon
    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, view);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, view);
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMSSQLDatabaseNode(MSSQLDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.purple));
    const ImVec2 msNodeMin = ImGui::GetItemRectMin();
    const ImVec2 msNodeMax = ImGui::GetItemRectMax();
    if (dbData->schemasLoaded && !dbData->schemas.empty()) {
        std::vector<std::string> schemaNames;
        schemaNames.reserve(dbData->schemas.size());
        for (const auto& s : dbData->schemas)
            if (s)
                schemaNames.push_back(s->name);
        renderSchemaFilterBadge(dbData->name, schemaNames, msNodeMin, msNodeMax,
                                static_cast<const void*>(dbData));
    }

    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete", [this, dbName]() { pendingDropDatabase_ = dbName; },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // load schemas if not loaded
        if (!dbData->schemasLoaded && !dbData->schemasLoader.isRunning()) {
            dbData->startSchemasLoadAsync();
        }

        if (dbData->schemasLoader.isRunning()) {
            dbData->checkSchemasStatusAsync();
            ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
            ImGui::TextUnformatted(LOADING_LABEL);
            ImGui::SameLine(0, Theme::Spacing::S);
            UIUtils::Spinner("##loading_schemas", 6.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::PopStyleColor();
        } else if (dbData->schemasLoaded) {
            for (auto& schema : dbData->schemas) {
                if (schema && isSchemaHidden(dbData->name, schema->name))
                    continue;
                renderMSSQLSchemaNode(dbData, schema.get());
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMSSQLSchemaNode(const MSSQLDatabaseNode* dbData,
                                              MSSQLSchemaNode* schemaData) {
    if (!dbData || !schemaData)
        return;

    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId =
        std::format("mssql_schema_{}_{:p}", schemaData->name, static_cast<void*>(schemaData));
    const bool isOpen = renderTreeNodeWithIcon(schemaData->name, nodeId, ICON_FK_FOLDER,
                                               ImGui::GetColorU32(colors.yellow));

    // context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", schemaData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(schemaData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            schemaData->startTablesLoadAsync(true);
            schemaData->startViewsLoadAsync(true);
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // tables
        {
            const std::string tablesNodeId = std::format("mssql_tables_{}_{:p}", schemaData->name,
                                                         static_cast<void*>(&schemaData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    app.getTabManager()->createTableEditorTab(schemaData, schemaData->name);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!schemaData->tablesLoaded && !schemaData->tablesLoader.isRunning()) {
                    schemaData->startTablesLoadAsync();
                }

                if (schemaData->tablesLoader.isRunning()) {
                    schemaData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->tablesLoaded) {
                    if (schemaData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : schemaData->tables) {
                            renderMSSQLTableNode(table, schemaData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // views
        {
            const std::string viewsNodeId = std::format("mssql_views_{}_{:p}", schemaData->name,
                                                        static_cast<void*>(&schemaData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!schemaData->viewsLoaded && !schemaData->viewsLoader.isRunning()) {
                    schemaData->startViewsLoadAsync();
                }

                if (schemaData->viewsLoader.isRunning()) {
                    schemaData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->viewsLoaded) {
                    if (schemaData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : schemaData->views) {
                            renderMSSQLViewNode(view, schemaData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Routines section
        {
            const std::string routineNodeId = std::format(
                "routines_{}_{:p}", schemaData->name, static_cast<void*>(&schemaData->routines));
            const bool routineOpen = renderTreeNodeWithIcon("Routines", routineNodeId, ICON_FA_CODE,
                                                            ImGui::GetColorU32(colors.yellow));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    schemaData->startRoutinesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (routineOpen) {
                if (!schemaData->routinesLoaded && !schemaData->routinesLoader.isRunning()) {
                    schemaData->startRoutinesLoadAsync();
                }

                if (schemaData->routinesLoader.isRunning()) {
                    schemaData->checkRoutinesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_routines", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (schemaData->routinesLoaded) {
                    if (schemaData->routines.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No routines");
                        ImGui::PopStyleColor();
                    } else {
                        renderRoutineItems(schemaData->routines, schemaData);
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMSSQLTableNode(Table& table, MSSQLSchemaNode* schemaData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&table);
    const bool isSelected = selectedTables_.count(&table) > 0;
    const ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                          ImGuiTreeNodeFlags_FramePadding |
                                          (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string tableNodeId =
        std::format("mssql_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        handleTableClick(&table);
    }

    const bool isRefreshing = schemaData->isTableRefreshing(table.name);
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        schemaData->checkTableRefreshStatusAsync(table.name);
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaData, table);
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool isMultiSelect = selectedTables_.size() > 1 && selectedTables_.count(&table) > 0;
        if (isMultiSelect) {
            renderMultiSelectMenuContent(
                schemaData, schemaData->getTables(),
                [schemaData](const std::string& n) { schemaData->dropTable(n); },
                schemaData->getDatabaseType());
        } else {
            if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
                app.getTabManager()->createTableViewerTab(schemaData, table);
            }
            if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
                app.getTabManager()->createTableEditorTab(schemaData, table);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                schemaData->startTableRefreshAsync(table.name);
            }
            TableExporter::renderExportMenu(schemaData, table, schemaData->getDatabaseType());
            TableImporter::renderImportMenu(schemaData, table.name);
            ImGui::Separator();
            if (ImGui::MenuItem(RENAME_LABEL)) {
                const std::string oldName = table.name;
                InputDialog::show(
                    "Rename Table", "New name:", oldName, "Rename",
                    [schemaData, oldName](const std::string& newName) -> std::string {
                        auto [success, error] = schemaData->renameTable(oldName, newName);
                        return success ? "" : error;
                    },
                    nullptr,
                    [oldName](const std::string& newName) -> std::string {
                        if (newName == oldName)
                            return "New name must be different";
                        return "";
                    });
            }
            if (ImGui::MenuItem(TRUNCATE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Truncate Table",
                    std::format("Remove all rows from '{}'? This is irreversible.", tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Truncate",
                      [schemaData, tableName]() {
                          auto [success, error] = schemaData->truncateTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to truncate table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
            if (ImGui::MenuItem(DELETE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Delete Table",
                    std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                                tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Delete",
                      [schemaData, tableName]() {
                          auto [success, error] = schemaData->dropTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to delete table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // columns
        {
            const std::string columnsNodeId = std::format("mssql_columns_{}_{:p}", table.name,
                                                          static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey)
                        columnDisplay += ", PK";
                    if (column.isNotNull)
                        columnDisplay += ", NOT NULL";

                    const std::string columnNodeId =
                        std::format("mssql_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [schemaData, tblName, colName]() {
                                      auto [success, error] =
                                          schemaData->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // foreign keys
        if (!table.foreignKeys.empty()) {
            const std::string fkNodeId =
                std::format("mssql_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                for (const auto& fk : table.foreignKeys) {
                    ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;
                    std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                        fk.targetTable, fk.targetColumn);
                    ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                    if (ImGui::IsItemHovered() && !fk.name.empty()) {
                        ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        // indexes
        if (!table.indexes.empty()) {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                for (const auto& index : table.indexes) {
                    ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_FramePadding;
                    std::string indexDisplay = index.name;
                    if (!index.columns.empty()) {
                        indexDisplay += " (";
                        for (size_t i = 0; i < index.columns.size(); ++i) {
                            if (i > 0)
                                indexDisplay += ", ";
                            indexDisplay += index.columns[i];
                        }
                        indexDisplay += ")";
                    }
                    if (index.isUnique)
                        indexDisplay += " UNIQUE";
                    ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                }
                ImGui::TreePop();
            }
        }

        // references (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMSSQLViewNode(Table& view, MSSQLSchemaNode* schemaData) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(schemaData, view);
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(schemaData, view);
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderOracleDatabaseNode(OracleDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.purple));

    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(SHOW_DIAGRAM_LABEL)) {
            app.getTabManager()->createDiagramTab(dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete", [this, dbName]() { pendingDropDatabase_ = dbName; },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // tables
        {
            const std::string tablesNodeId =
                std::format("tables_{}_{:p}", dbData->name, static_cast<void*>(&dbData->tables));
            const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                           ImGui::GetColorU32(colors.green));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(CREATE_TABLE_LABEL)) {
                    app.getTabManager()->createTableEditorTab(dbData);
                }
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startTablesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (tablesOpen) {
                if (!dbData->tablesLoaded && !dbData->tablesLoader.isRunning()) {
                    dbData->startTablesLoadAsync();
                }

                if (dbData->tablesLoader.isRunning()) {
                    dbData->checkTablesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_tables", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->tablesLoaded) {
                    if (dbData->tables.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No tables");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& table : dbData->tables) {
                            renderOracleTableNode(table, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // views
        {
            const std::string viewsNodeId =
                std::format("views_{}_{:p}", dbData->name, static_cast<void*>(&dbData->views));
            const bool viewsOpen = renderTreeNodeWithIcon("Views", viewsNodeId, ICON_FK_EYE,
                                                          ImGui::GetColorU32(colors.teal));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startViewsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (viewsOpen) {
                if (!dbData->viewsLoaded && !dbData->viewsLoader.isRunning()) {
                    dbData->startViewsLoadAsync();
                }

                if (dbData->viewsLoader.isRunning()) {
                    dbData->checkViewsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->viewsLoaded) {
                    if (dbData->views.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No views");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& view : dbData->views) {
                            renderOracleViewNode(view, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        // Render Routines section
        {
            const std::string routineNodeId = std::format("routines_{}_{:p}", dbData->name,
                                                          static_cast<void*>(&dbData->routines));
            const bool routineOpen = renderTreeNodeWithIcon("Routines", routineNodeId, ICON_FA_CODE,
                                                            ImGui::GetColorU32(colors.yellow));

            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startRoutinesLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (routineOpen) {
                if (!dbData->routinesLoaded && !dbData->routinesLoader.isRunning()) {
                    dbData->startRoutinesLoadAsync();
                }

                if (dbData->routinesLoader.isRunning()) {
                    dbData->checkRoutinesStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_routines", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->routinesLoaded) {
                    if (dbData->routines.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No routines");
                        ImGui::PopStyleColor();
                    } else {
                        renderRoutineItems(dbData->routines, dbData);
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderOracleTableNode(Table& table, OracleDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&table);
    const bool isSelected = selectedTables_.count(&table) > 0;
    const ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                          ImGuiTreeNodeFlags_FramePadding |
                                          (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string tableNodeId =
        std::format("oracle_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        handleTableClick(&table);
    }

    const bool isRefreshing = dbData->isTableRefreshing(table.name);
    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_table_{}", table.name).c_str(), spinnerRadius, 2,
                         ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        dbData->checkTableRefreshStatusAsync(table.name);
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, table);
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool isMultiSelect = selectedTables_.size() > 1 && selectedTables_.count(&table) > 0;
        if (isMultiSelect) {
            renderMultiSelectMenuContent(
                dbData, dbData->getTables(),
                [dbData](const std::string& n) { dbData->dropTable(n); },
                dbData->getDatabaseType());
        } else {
            if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
                app.getTabManager()->createTableViewerTab(dbData, table);
            }
            if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
                app.getTabManager()->createTableEditorTab(dbData, table);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                dbData->startTableRefreshAsync(table.name);
            }
            TableExporter::renderExportMenu(dbData, table, dbData->getDatabaseType());
            TableImporter::renderImportMenu(dbData, table.name);
            ImGui::Separator();
            if (ImGui::MenuItem(RENAME_LABEL)) {
                const std::string oldName = table.name;
                InputDialog::show(
                    "Rename Table", "New name:", oldName, "Rename",
                    [dbData, oldName](const std::string& newName) -> std::string {
                        auto [success, error] = dbData->renameTable(oldName, newName);
                        return success ? "" : error;
                    },
                    nullptr,
                    [oldName](const std::string& newName) -> std::string {
                        if (newName == oldName)
                            return "New name must be different";
                        return "";
                    });
            }
            if (ImGui::MenuItem(TRUNCATE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Truncate Table",
                    std::format("Remove all rows from '{}'? This is irreversible.", tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Truncate",
                      [dbData, tableName]() {
                          auto [success, error] = dbData->truncateTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to truncate table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
            if (ImGui::MenuItem(DELETE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Delete Table",
                    std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                                tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Delete",
                      [dbData, tableName]() {
                          auto [success, error] = dbData->dropTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to delete table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // columns
        {
            const std::string columnsNodeId = std::format("oracle_columns_{}_{:p}", table.name,
                                                          static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey)
                        columnDisplay += ", PK";
                    if (column.isNotNull)
                        columnDisplay += ", NOT NULL";

                    const std::string columnNodeId =
                        std::format("oracle_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [dbData, tblName, colName]() {
                                      auto [success, error] = dbData->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // foreign keys
        if (!table.foreignKeys.empty()) {
            const std::string fkNodeId =
                std::format("oracle_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                for (const auto& fk : table.foreignKeys) {
                    ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;
                    std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                        fk.targetTable, fk.targetColumn);
                    ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                    if (ImGui::IsItemHovered() && !fk.name.empty()) {
                        ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        // indexes
        if (!table.indexes.empty()) {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                for (const auto& index : table.indexes) {
                    ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                    ImGuiTreeNodeFlags_FramePadding;
                    std::string indexDisplay = index.name;
                    if (!index.columns.empty()) {
                        indexDisplay += " (";
                        for (size_t i = 0; i < index.columns.size(); ++i) {
                            if (i > 0)
                                indexDisplay += ", ";
                            indexDisplay += index.columns[i];
                        }
                        indexDisplay += ")";
                    }
                    if (index.isUnique)
                        indexDisplay += " UNIQUE";
                    ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                }
                ImGui::TreePop();
            }
        }

        // references (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderOracleViewNode(Table& view, OracleDatabaseNode* dbData) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, view);
    }

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(dbData, view);
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderMongoDBDatabaseNode(MongoDBDatabaseNode* dbData) {
    if (!dbData) {
        return;
    }

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.green));

    if (ImGui::IsItemToggledOpen()) {
        dbData->expanded = isOpen;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_QUERY_EDITOR_LABEL)) {
            app.getTabManager()->createMongoEditorTab(dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startCollectionsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string dbName = dbData->name;
            Alert::show(
                "Delete Database",
                std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                            dbName),
                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                 {"Delete", [this, dbName]() { pendingDropDatabase_ = dbName; },
                  AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        // Render Collections section
        {
            const std::string collectionsNodeId = std::format(
                "collections_{}_{:p}", dbData->name, static_cast<void*>(&dbData->collections));
            const bool collectionsOpen = renderTreeNodeWithIcon(
                "Collections", collectionsNodeId, ICON_FK_TABLE, ImGui::GetColorU32(colors.green));

            // Context menu for Collections node
            if (ImGui::BeginPopupContextItem(nullptr)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                if (ImGui::MenuItem(REFRESH_LABEL)) {
                    dbData->startCollectionsLoadAsync(true);
                }
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (collectionsOpen) {
                if (!dbData->collectionsLoaded && !dbData->collectionsLoader.isRunning()) {
                    dbData->startCollectionsLoadAsync();
                }

                if (dbData->collectionsLoader.isRunning()) {
                    dbData->checkCollectionsStatusAsync();
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                    ImGui::TextUnformatted(LOADING_LABEL);
                    ImGui::SameLine(0, Theme::Spacing::S);
                    UIUtils::Spinner("##loading_collections", 6.0f, 2,
                                     ImGui::GetColorU32(colors.peach));
                    ImGui::PopStyleColor();
                } else if (dbData->collectionsLoaded) {
                    if (dbData->collections.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                        ImGui::Text("  No collections");
                        ImGui::PopStyleColor();
                    } else {
                        for (auto& collection : dbData->collections) {
                            renderMongoDBCollectionNode(collection, dbData);
                        }
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderMongoDBCollectionNode(Table& collection,
                                                    MongoDBDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&collection);
    const bool isSelected = selectedTables_.count(&collection) > 0;
    const ImGuiTreeNodeFlags collectionFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                               ImGuiTreeNodeFlags_FramePadding |
                                               (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string collectionNodeId =
        std::format("mongo_coll_{}_{:p}", collection.name, static_cast<const void*>(&collection));
    const bool collectionOpen =
        renderTreeNodeWithIcon(collection.name, collectionNodeId, ICON_FK_TABLE,
                               ImGui::GetColorU32(colors.green), collectionFlags);

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        handleTableClick(&collection);
    }

    // Check if collection is refreshing
    const bool isRefreshing = dbData->isTableRefreshing(collection.name);

    if (isRefreshing) {
        constexpr float spinnerRadius = 6.0f;
        const float spinnerX = ImGui::GetItemRectMax().x + 4.0f;
        const float itemCenterY = ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y * 0.5f);
        const float spinnerY = itemCenterY - spinnerRadius - ImGui::GetStyle().FramePadding.y;
        ImGui::SetCursorScreenPos(ImVec2(spinnerX, spinnerY));

        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        UIUtils::Spinner(std::format("##refreshing_coll_{}", collection.name).c_str(),
                         spinnerRadius, 2, ImGui::GetColorU32(colors.peach));
        ImGui::PopStyleColor();

        dbData->checkTableRefreshStatusAsync(collection.name);
    }

    // Double-click to open collection viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(dbData, collection);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool isMultiSelect =
            selectedTables_.size() > 1 && selectedTables_.count(&collection) > 0;
        if (isMultiSelect) {
            renderMultiSelectMenuContent(
                dbData, dbData->getTables(),
                [dbData](const std::string& n) { dbData->dropCollection(n); },
                dbData->getDatabaseType());
        } else {
            if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
                app.getTabManager()->createTableViewerTab(dbData, collection);
            }
            if (ImGui::MenuItem(REFRESH_LABEL)) {
                dbData->startTableRefreshAsync(collection.name);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(DELETE_LABEL)) {
                const std::string collName = collection.name;
                Alert::show(
                    "Delete Collection",
                    std::format(
                        "Permanently delete '{}' and ALL its documents? This is irreversible.",
                        collName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Delete",
                      [dbData, collName]() {
                          auto [success, error] = dbData->dropCollection(collName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to delete collection: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (collectionOpen) {
        // Fields section (inferred schema)
        {
            const std::string fieldsNodeId = std::format("mongo_fields_{}_{:p}", collection.name,
                                                         static_cast<void*>(&collection.columns));
            const bool fieldsOpen = renderTreeNodeWithIcon(
                "Fields", fieldsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (fieldsOpen) {
                if (collection.columns.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No fields detected");
                    ImGui::PopStyleColor();
                } else {
                    for (const auto& column : collection.columns) {
                        ImGuiTreeNodeFlags fieldFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;

                        std::string fieldDisplay = std::format("{} ({})", column.name, column.type);
                        if (column.isPrimaryKey) {
                            fieldDisplay += ", PK";
                        }

                        const std::string fieldNodeId =
                            std::format("mongo_field_{}_{}_{:p}", collection.name, column.name,
                                        static_cast<const void*>(&column));
                        const std::string fieldLabel =
                            std::format("{}###{}", fieldDisplay, fieldNodeId);
                        ImGui::TreeNodeEx(fieldLabel.c_str(), fieldFlags);
                    }
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId = std::format("mongo_indexes_{}_{:p}", collection.name,
                                                          static_cast<void*>(&collection.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!collection.indexes.empty()) {
                    for (const auto& index : collection.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderSQLiteTableNode(Table& table, SQLiteDatabase* sqliteDb) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&table);
    const bool isSelected = selectedTables_.count(&table) > 0;
    const ImGuiTreeNodeFlags tableFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                          ImGuiTreeNodeFlags_FramePadding |
                                          (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string tableNodeId =
        std::format("sqlite_table_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool tableOpen = renderTreeNodeWithIcon(table.name, tableNodeId, ICON_FK_TABLE,
                                                  ImGui::GetColorU32(colors.green), tableFlags);

    // dim size badge anchored to the right edge of the sidebar
    if (table.sizeBytes >= 0) {
        const std::string sizeText = formatByteSize(table.sizeBytes);
        const ImVec2 textSize = ImGui::CalcTextSize(sizeText.c_str());
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const float yCenter =
            itemMin.y + (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f;
        const ImVec2 textPos = ImVec2(rightEdge - textSize.x - Theme::Spacing::M, yCenter);
        ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(colors.subtext0),
                                            sizeText.c_str());
    }

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        handleTableClick(&table);
    }

    // Double-click to open table viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(sqliteDb, table);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        const bool isMultiSelect = selectedTables_.size() > 1 && selectedTables_.count(&table) > 0;
        if (isMultiSelect) {
            renderMultiSelectMenuContent(
                sqliteDb, sqliteDb->getTables(),
                [sqliteDb](const std::string& n) { sqliteDb->dropTable(n); });
        } else {
            if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
                app.getTabManager()->createTableViewerTab(sqliteDb, table);
            }
            if (ImGui::MenuItem(EDIT_TABLE_LABEL)) {
                app.getTabManager()->createTableEditorTab(sqliteDb, table);
            }
            TableExporter::renderExportMenu(sqliteDb, table);
            TableImporter::renderImportMenu(sqliteDb, table.name);
            ImGui::Separator();
            if (ImGui::MenuItem(RENAME_LABEL)) {
                const std::string oldName = table.name;
                InputDialog::show(
                    "Rename Table", "New name:", oldName, "Rename",
                    [sqliteDb, oldName](const std::string& newName) -> std::string {
                        auto [success, error] = sqliteDb->renameTable(oldName, newName);
                        return success ? "" : error;
                    },
                    nullptr,
                    [oldName](const std::string& newName) -> std::string {
                        if (newName == oldName)
                            return "New name must be different";
                        return "";
                    });
            }
            if (ImGui::MenuItem(DELETE_LABEL)) {
                const std::string tableName = table.name;
                Alert::show(
                    "Delete Table",
                    std::format("Permanently delete '{}' and ALL its data? This is irreversible.",
                                tableName),
                    {{"Cancel", nullptr, AlertButton::Style::Cancel},
                     {"Delete",
                      [sqliteDb, tableName]() {
                          auto [success, error] = sqliteDb->dropTable(tableName);
                          if (!success) {
                              Alert::show("Error",
                                          std::format("Failed to delete table: {}", error));
                          }
                      },
                      AlertButton::Style::Destructive}});
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (tableOpen) {
        // Columns section
        {
            const std::string columnsNodeId =
                std::format("columns_{}_{:p}", table.name, static_cast<void*>(&table.columns));
            const bool columnsOpen = renderTreeNodeWithIcon(
                "Columns", columnsNodeId, ICON_FA_TABLE_COLUMNS, ImGui::GetColorU32(colors.green));

            if (columnsOpen) {
                for (const auto& column : table.columns) {
                    ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;

                    std::string columnDisplay = std::format("{} ({})", column.name, column.type);
                    if (column.isPrimaryKey) {
                        columnDisplay += ", PK";
                    }
                    if (column.isNotNull) {
                        columnDisplay += ", NOT NULL";
                    }

                    const std::string columnNodeId =
                        std::format("sqlite_col_{}_{}_{:p}", table.name, column.name,
                                    static_cast<const void*>(&column));
                    const std::string columnLabel =
                        std::format("{}###{}", columnDisplay, columnNodeId);
                    ImGui::TreeNodeEx(columnLabel.c_str(), columnFlags);

                    // Context menu for column
                    if (ImGui::BeginPopupContextItem(columnNodeId.c_str())) {
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                        if (ImGui::MenuItem(DELETE_LABEL)) {
                            const std::string colName = column.name;
                            const std::string tblName = table.name;
                            Alert::show(
                                "Drop Column",
                                std::format("Permanently drop column '{}.{}'?", tblName, colName),
                                {{"Cancel", nullptr, AlertButton::Style::Cancel},
                                 {"Drop",
                                  [sqliteDb, tblName, colName]() {
                                      auto [success, error] =
                                          sqliteDb->dropColumn(tblName, colName);
                                      if (!success) {
                                          Alert::show(
                                              "Error",
                                              std::format("Failed to drop column: {}", error));
                                      }
                                  },
                                  AlertButton::Style::Destructive}});
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }
                }
                ImGui::TreePop();
            }
        }

        // Foreign Keys section
        {
            const std::string fkNodeId =
                std::format("sqlite_foreign_keys_{}_{:p}", table.name, static_cast<void*>(&table));
            const bool fkOpen = renderTreeNodeWithIcon("Foreign Keys", fkNodeId, ICON_FA_KEY,
                                                       ImGui::GetColorU32(colors.yellow));

            if (fkOpen) {
                if (!table.foreignKeys.empty()) {
                    for (const auto& fk : table.foreignKeys) {
                        ImGuiTreeNodeFlags fkFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
                        std::string fkDisplay = std::format("{} -> {}.{}", fk.sourceColumn,
                                                            fk.targetTable, fk.targetColumn);
                        ImGui::TreeNodeEx(fkDisplay.c_str(), fkFlags);

                        if (ImGui::IsItemHovered() && !fk.name.empty()) {
                            ImGui::SetTooltip("Constraint: %s", fk.name.c_str());
                        }
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No foreign keys");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // Indexes section
        {
            const std::string indexesNodeId =
                std::format("indexes_{}_{:p}", table.name, static_cast<void*>(&table.indexes));
            const bool indexesOpen =
                renderTreeNodeWithIcon("Indexes", indexesNodeId, ICON_FA_MAGNIFYING_GLASS,
                                       ImGui::GetColorU32(colors.lavender));

            if (indexesOpen) {
                if (!table.indexes.empty()) {
                    for (const auto& index : table.indexes) {
                        ImGuiTreeNodeFlags indexFlags = ImGuiTreeNodeFlags_Leaf |
                                                        ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                        ImGuiTreeNodeFlags_FramePadding;
                        std::string indexDisplay = index.name;
                        if (!index.columns.empty()) {
                            indexDisplay += " (";
                            for (size_t i = 0; i < index.columns.size(); ++i) {
                                if (i > 0)
                                    indexDisplay += ", ";
                                indexDisplay += index.columns[i];
                            }
                            indexDisplay += ")";
                        }
                        if (index.isUnique) {
                            indexDisplay += " UNIQUE";
                        }
                        ImGui::TreeNodeEx(indexDisplay.c_str(), indexFlags);
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No indexes defined");
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
        }

        // References section (incoming foreign keys)
        if (!table.incomingForeignKeys.empty()) {
            const std::string referencesNodeId = std::format(
                "references_{}_{:p}", table.name, static_cast<void*>(&table.incomingForeignKeys));
            const bool referencesOpen = renderTreeNodeWithIcon("References", referencesNodeId,
                                                               ICON_FA_ARROW_RIGHT_TO_BRACKET,
                                                               ImGui::GetColorU32(colors.sky));

            if (referencesOpen) {
                for (const auto& ref : table.incomingForeignKeys) {
                    ImGuiTreeNodeFlags refFlags = ImGuiTreeNodeFlags_Leaf |
                                                  ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                  ImGuiTreeNodeFlags_FramePadding;
                    std::string refDisplay =
                        std::format("{}.{}", ref.targetTable, ref.sourceColumn);
                    ImGui::TreeNodeEx(refDisplay.c_str(), refFlags);

                    if (ImGui::IsItemHovered()) {
                        std::string tooltip =
                            std::format("{}.{} -> {}.{}", ref.targetTable, ref.sourceColumn,
                                        table.name, ref.targetColumn);
                        if (!ref.name.empty()) {
                            tooltip += std::format("\nConstraint: {}", ref.name);
                        }
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

IDatabaseNode* DatabaseHierarchy::resolveNodeForQuery(const SqlScript& query) const {
    if (!db)
        return nullptr;

    const auto dbType = db->getConnectionInfo().type;

    if (dbType == DatabaseType::SQLITE) {
        return dynamic_cast<SQLiteDatabase*>(db.get());
    }
    if (dbType == DatabaseType::POSTGRESQL || dbType == DatabaseType::REDSHIFT) {
        auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get());
        if (!pgDb)
            return nullptr;
        if (auto* dbNode = pgDb->getDatabaseData(query.databaseName)) {
            if (!query.schemaName.empty()) {
                for (const auto& schema : dbNode->schemas) {
                    if (schema && schema->name == query.schemaName)
                        return schema.get();
                }
            }
            return dbNode;
        }
        return nullptr;
    }
    if (dbType == DatabaseType::MYSQL || dbType == DatabaseType::MARIADB) {
        auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get());
        if (!mysqlDb)
            return nullptr;
        return mysqlDb->getDatabaseData(query.databaseName);
    }
    if (dbType == DatabaseType::MSSQL) {
        auto* mssqlDb = dynamic_cast<MSSQLDatabase*>(db.get());
        if (!mssqlDb)
            return nullptr;
        if (auto* dbNode = mssqlDb->getDatabaseData(query.databaseName)) {
            if (!query.schemaName.empty()) {
                for (const auto& schema : dbNode->schemas) {
                    if (schema && schema->name == query.schemaName)
                        return schema.get();
                }
            }
            return dbNode;
        }
        return nullptr;
    }
    if (dbType == DatabaseType::ORACLE) {
        auto* oracleDb = dynamic_cast<OracleDatabase*>(db.get());
        if (!oracleDb)
            return nullptr;
        return oracleDb->getDatabaseData(query.databaseName);
    }
    return nullptr;
}

void DatabaseHierarchy::renderQueriesNode() {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const int connId = db->getConnectionId();
    if (connId <= 0)
        return;

    auto* appState = app.getAppState();
    if (!appState)
        return;

    const auto queries = appState->getScriptsForConnection(connId);

    const std::string queriesNodeId = std::format("scripts_conn_{}", static_cast<void*>(db.get()));
    const bool queriesOpen = renderTreeNodeWithIcon("Queries", queriesNodeId, ICON_FA_FILE_CODE,
                                                    ImGui::GetColorU32(colors.purple));

    // context menu on the Queriess header
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem("New SQL Query")) {
            // resolve the first available database node for this connection
            IDatabaseNode* node = nullptr;
            const auto dbType = db->getConnectionInfo().type;
            if (auto* sqliteDb = dynamic_cast<SQLiteDatabase*>(db.get())) {
                node = sqliteDb;
            } else if (dbType == DatabaseType::POSTGRESQL || dbType == DatabaseType::REDSHIFT) {
                if (auto* pgDb = dynamic_cast<PostgresDatabase*>(db.get())) {
                    for (const auto& entry : pgDb->getDatabaseDataMap()) {
                        if (entry.second && !entry.second->schemas.empty() &&
                            entry.second->schemas.front()) {
                            node = entry.second->schemas.front().get();
                            break;
                        }
                        if (entry.second) {
                            node = entry.second.get();
                            break;
                        }
                    }
                }
            } else if (dbType == DatabaseType::MYSQL || dbType == DatabaseType::MARIADB) {
                if (auto* mysqlDb = dynamic_cast<MySQLDatabase*>(db.get())) {
                    // prefer a real database over the schema-less server node
                    for (const auto& entry : mysqlDb->getDatabaseDataMap()) {
                        if (entry.second && !entry.first.empty()) {
                            node = entry.second.get();
                            break;
                        }
                    }
                }
            } else if (dbType == DatabaseType::MSSQL) {
                if (auto* mssqlDb = dynamic_cast<MSSQLDatabase*>(db.get())) {
                    const auto& m = mssqlDb->getDatabaseDataMap();
                    if (!m.empty() && m.begin()->second)
                        node = m.begin()->second.get();
                }
            } else if (dbType == DatabaseType::ORACLE) {
                if (auto* oracleDb = dynamic_cast<OracleDatabase*>(db.get())) {
                    const auto& m = oracleDb->getDatabaseDataMap();
                    if (!m.empty() && m.begin()->second)
                        node = m.begin()->second.get();
                }
            }
            if (node) {
                app.getTabManager()->createSQLEditorTab("", node);
            } else {
                spdlog::warn("New SQL Query: no loaded database node found for this connection");
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (queriesOpen) {
        if (queries.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::Text("  No query");
            ImGui::PopStyleColor();
        } else {
            constexpr ImGuiTreeNodeFlags queryItemFlags = ImGuiTreeNodeFlags_Leaf |
                                                          ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                          ImGuiTreeNodeFlags_FramePadding;
            for (const auto& query : queries) {
                const std::string queryItemId =
                    std::format("script_{}_{}", query.id, static_cast<const void*>(&query));
                renderTreeNodeWithIcon(query.name, queryItemId, ICON_FA_FILE_CODE,
                                       ImGui::GetColorU32(colors.purple), queryItemFlags);

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    IDatabaseNode* node = resolveNodeForQuery(query);
                    app.getTabManager()->createSQLEditorTabFromQuery(node, query);
                }

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", query.filePath.c_str());
                }

                if (ImGui::BeginPopupContextItem(queryItemId.c_str())) {
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                        ImVec2(Theme::Spacing::M, Theme::Spacing::M));
                    if (ImGui::MenuItem("Open")) {
                        IDatabaseNode* node = resolveNodeForQuery(query);
                        app.getTabManager()->createSQLEditorTabFromQuery(node, query);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete")) {
                        const int queryId = query.id;
                        Alert::show(
                            "Delete Query",
                            std::format("Remove '{}' from the list? The file won't be deleted.",
                                        query.name),
                            {{"Cancel", nullptr, AlertButton::Style::Cancel},
                             {"Remove", [appState, queryId]() { appState->deleteScript(queryId); },
                              AlertButton::Style::Destructive}});
                    }
                    ImGui::PopStyleVar();
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderSQLiteViewNode(Table& view, SQLiteDatabase* sqliteDb) {
    const auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    constexpr ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf |
                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                             ImGuiTreeNodeFlags_FramePadding;

    const std::string viewNodeId =
        std::format("view_{}_{:p}", view.name, static_cast<void*>(&view));
    const std::string viewLabel = std::format("   {}###{}", view.name, viewNodeId);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    // Draw icon
    const auto iconPos =
        ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
               ImGui::GetItemRectMin().y +
                   (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(iconPos, ImGui::GetColorU32(colors.teal), ICON_FK_EYE);

    // Double-click to open view viewer
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(sqliteDb, view);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL)) {
            app.getTabManager()->createTableViewerTab(sqliteDb, view);
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

void DatabaseHierarchy::renderCassandraDatabaseNode(CassandraDatabaseNode* dbData) {
    if (!dbData)
        return;

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    const std::string nodeId = std::format("ks_{}_{:p}", dbData->name, static_cast<void*>(dbData));
    const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                               ImGui::GetColorU32(colors.blue));

    if (ImGui::IsItemToggledOpen())
        dbData->expanded = isOpen;

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(NEW_SQL_EDITOR_LABEL)) {
            app.getTabManager()->createSQLEditorTab("", dbData);
        }
        if (ImGui::MenuItem(REFRESH_LABEL)) {
            dbData->startTablesLoadAsync(true);
            dbData->startViewsLoadAsync(true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string ksName = dbData->name;
            Alert::show("Drop Keyspace",
                        std::format("Permanently drop keyspace '{}' and ALL its data?", ksName),
                        {{"Cancel", nullptr, AlertButton::Style::Cancel},
                         {"Drop", [this, ksName]() { pendingDropDatabase_ = ksName; },
                          AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (!isOpen)
        return;

    // Tables
    {
        const std::string tablesNodeId =
            std::format("cass_tables_{}_{:p}", dbData->name, static_cast<void*>(&dbData->tables));
        const bool tablesOpen = renderTreeNodeWithIcon("Tables", tablesNodeId, ICON_FK_TABLE,
                                                       ImGui::GetColorU32(colors.green));

        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(CREATE_TABLE_LABEL))
                app.getTabManager()->createTableEditorTab(dbData, dbData->name);
            if (ImGui::MenuItem(REFRESH_LABEL))
                dbData->startTablesLoadAsync(true);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (tablesOpen) {
            if (!dbData->tablesLoaded && !dbData->tablesLoader.isRunning())
                dbData->startTablesLoadAsync();

            if (dbData->tablesLoader.isRunning()) {
                dbData->checkLoadingStatus();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::TextUnformatted(LOADING_LABEL);
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_cass_tables", 6.0f, 2,
                                 ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (dbData->tablesLoaded) {
                if (dbData->tables.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No tables");
                    ImGui::PopStyleColor();
                } else {
                    for (auto& t : dbData->tables)
                        renderCassandraTableNode(t, dbData);
                }
            }
            ImGui::TreePop();
        }
    }

    // Materialized views (CQL has no plain views).
    {
        const std::string viewsNodeId =
            std::format("cass_views_{}_{:p}", dbData->name, static_cast<void*>(&dbData->views));
        const bool viewsOpen = renderTreeNodeWithIcon("Materialized Views", viewsNodeId,
                                                      ICON_FK_EYE, ImGui::GetColorU32(colors.teal));

        if (ImGui::BeginPopupContextItem(nullptr)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem(REFRESH_LABEL))
                dbData->startViewsLoadAsync(true);
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }

        if (viewsOpen) {
            if (!dbData->viewsLoaded && !dbData->viewsLoader.isRunning())
                dbData->startViewsLoadAsync();

            if (dbData->viewsLoader.isRunning()) {
                dbData->checkLoadingStatus();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::TextUnformatted(LOADING_LABEL);
                ImGui::SameLine(0, Theme::Spacing::S);
                UIUtils::Spinner("##loading_cass_views", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (dbData->viewsLoaded) {
                if (dbData->views.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::Text("  No views");
                    ImGui::PopStyleColor();
                } else {
                    for (auto& v : dbData->views)
                        renderCassandraViewNode(v, dbData);
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::TreePop();
}

void DatabaseHierarchy::renderCassandraTableNode(Table& table, CassandraDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&table);
    const bool isSelected = selectedTables_.count(&table) > 0;
    const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_FramePadding |
                                     (isSelected ? ImGuiTreeNodeFlags_Selected : 0);

    const std::string nodeId =
        std::format("cass_tbl_{}_{:p}", table.name, static_cast<const void*>(&table));
    const bool isOpen = renderTreeNodeWithIcon(table.name, nodeId, ICON_FK_TABLE,
                                               ImGui::GetColorU32(colors.green), flags);

    if (ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen())
        handleTableClick(&table);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        app.getTabManager()->createTableViewerTab(dbData, table);

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL))
            app.getTabManager()->createTableViewerTab(dbData, table);
        if (ImGui::MenuItem(REFRESH_LABEL))
            dbData->startTableRefreshAsync(table.name);
        ImGui::Separator();
        if (ImGui::MenuItem(DELETE_LABEL)) {
            const std::string tName = table.name;
            Alert::show("Drop Table", std::format("Permanently drop table '{}'?", tName),
                        {{"Cancel", nullptr, AlertButton::Style::Cancel},
                         {"Drop",
                          [dbData, tName]() {
                              auto [ok, err] = dbData->dropTable(tName);
                              if (!ok)
                                  Alert::show("Error",
                                              std::format("Failed to drop table: {}", err));
                          },
                          AlertButton::Style::Destructive}});
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (dbData->isTableRefreshing(table.name))
        dbData->checkTableRefreshStatusAsync(table.name);

    if (isOpen) {
        const std::string colsId =
            std::format("cass_cols_{}_{:p}", table.name, static_cast<void*>(&table.columns));
        const bool colsOpen = renderTreeNodeWithIcon("Columns", colsId, ICON_FA_TABLE_COLUMNS,
                                                     ImGui::GetColorU32(colors.green));

        if (colsOpen) {
            if (table.columns.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                ImGui::Text("  No columns");
                ImGui::PopStyleColor();
            } else {
                for (const auto& col : table.columns) {
                    constexpr ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf |
                                                             ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                             ImGuiTreeNodeFlags_FramePadding;
                    std::string label = std::format("{} ({})", col.name, col.type);
                    if (col.isPrimaryKey)
                        label += ", PK";
                    const std::string id = std::format("###cass_col_{}_{:p}", col.name,
                                                       static_cast<const void*>(&col));
                    ImGui::TreeNodeEx((label + id).c_str(), leafFlags);
                }
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
}

void DatabaseHierarchy::renderCassandraViewNode(Table& view, CassandraDatabaseNode* dbData) {
    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    currVisibleTables_.push_back(&view);
    const ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;

    const std::string nodeId =
        std::format("cass_view_{}_{:p}", view.name, static_cast<const void*>(&view));
    const bool isOpen = renderTreeNodeWithIcon(view.name, nodeId, ICON_FK_EYE,
                                               ImGui::GetColorU32(colors.teal), flags);

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        app.getTabManager()->createTableViewerTab(dbData, view);

    if (ImGui::BeginPopupContextItem(nullptr)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        if (ImGui::MenuItem(VIEW_DATA_LABEL))
            app.getTabManager()->createTableViewerTab(dbData, view);
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (isOpen) {
        for (const auto& col : view.columns) {
            constexpr ImGuiTreeNodeFlags leafFlags = ImGuiTreeNodeFlags_Leaf |
                                                     ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                     ImGuiTreeNodeFlags_FramePadding;
            std::string label = std::format("{} ({})", col.name, col.type);
            const std::string id =
                std::format("###cass_vcol_{}_{:p}", col.name, static_cast<const void*>(&col));
            ImGui::TreeNodeEx((label + id).c_str(), leafFlags);
        }
        ImGui::TreePop();
    }
}
