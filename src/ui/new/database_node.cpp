#include "ui/new/database_node.hpp"
#include "IconsForkAwesome.h"
#include "application.hpp"
#include "imgui.h"
#include "utils/logger.hpp"
#include "utils/spinner.hpp"
#include <format>
#include <ranges>

namespace NewHierarchy {
    namespace {
        // helper function to render a tree node with icon
        bool
        renderTreeNodeWithIcon(const std::string& label, const std::string& nodeId,
                               const std::string& icon, ImU32 iconColor,
                               ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                          ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                          ImGuiTreeNodeFlags_FramePadding) {
            const std::string fullLabel = std::format("   {}###{}", label, nodeId);
            const bool isOpen = ImGui::TreeNodeEx(fullLabel.c_str(), flags);

            // Draw icon
            const auto iconPos =
                ImVec2(ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing(),
                       ImGui::GetItemRectMin().y +
                           (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(iconPos, iconColor, icon.c_str());

            return isOpen;
        }
    } // namespace

    void renderRootDatabaseNode(const std::shared_ptr<DatabaseInterface>& dbInterface) {
        if (!dbInterface) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        // Get database type
        const auto dbType = dbInterface->getType();

        if (dbType == DatabaseType::SQLITE) {
            // SQLite: direct tables/views rendering (no multi-database support)
            // TODO: Implement SQLite hierarchy
            ImGui::Text("SQLite rendering");
        } else if (dbType == DatabaseType::POSTGRESQL) {
            auto* pgDb = dynamic_cast<PostgresDatabase*>(dbInterface.get());
            if (!pgDb) {
                return;
            }

            if (!pgDb->areDatabasesLoaded() && !pgDb->isLoadingDatabases()) {
                pgDb->refreshDatabaseNames();
            }

            if (pgDb->isLoadingDatabases()) {
                pgDb->checkDatabasesStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::Text("  Loading databases...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (pgDb->areDatabasesLoaded()) {
                const auto& databases = pgDb->getDatabaseDataMap() | std::views::values;
                for (const auto& dbData : databases) {
                    renderPostgresDatabaseNode(
                        pgDb, const_cast<PostgresDatabase::DatabaseData*>(&dbData));
                }
            }
        } else if (dbType == DatabaseType::MYSQL) {
            auto* mysqlDb = dynamic_cast<MySQLDatabase*>(dbInterface.get());
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
                ImGui::Text("  Loading databases...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_dbs_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (mysqlDb->areDatabasesLoaded()) {
                const auto& databases = mysqlDb->getDatabaseDataMap() | std::views::values;
                for (const auto& dbData : databases) {
                    renderMySQLDatabaseNode(mysqlDb,
                                            const_cast<MySQLDatabase::DatabaseData*>(&dbData));
                }
            }
        }
    }

    // Database-specific rendering implementations
    void renderPostgresDatabaseNode(PostgresDatabase* pgDb,
                                    PostgresDatabase::DatabaseData* dbData) {
        if (!pgDb || !dbData) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        const std::string nodeId =
            std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
        const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                                   ImGui::GetColorU32(colors.blue));

        // Handle expand/collapse
        if (ImGui::IsItemToggledOpen()) {
            dbData->expanded = isOpen;
        }

        if (isOpen) {
            // PostgreSQL: render schemas
            if (!dbData->schemasLoaded && !dbData->loadingSchemas) {
                dbData->startSchemasLoadAsync();
            }

            if (dbData->loadingSchemas) {
                dbData->checkSchemasStatusAsync();
                ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                ImGui::Text("  Loading schemas...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_schemas", 6.0f, 2, ImGui::GetColorU32(colors.peach));
                ImGui::PopStyleColor();
            } else if (dbData->schemasLoaded) {
                // Render each schema
                for (auto& [schemaName, schemaData] : dbData->schemaDataCache) {
                    renderPostgresSchemaNode(pgDb, dbData, &schemaData);
                }
            }

            ImGui::TreePop();
        }
    }

    void renderPostgresSchemaNode(PostgresDatabase* pgDb, PostgresDatabase::DatabaseData* dbData,
                                  PostgresDatabase::SchemaData* schemaData) {
        if (!pgDb || !dbData || !schemaData) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        const std::string nodeId =
            std::format("schema_{}_{:p}", schemaData->name, static_cast<void*>(schemaData));
        const bool isOpen = renderTreeNodeWithIcon(schemaData->name, nodeId, ICON_FK_FOLDER,
                                                   ImGui::GetColorU32(colors.yellow));

        if (isOpen) {
            // Render Tables section
            {
                const std::string tablesNodeId = std::format(
                    "tables_{}_{:p}", schemaData->name, static_cast<void*>(&schemaData->tables));
                const bool tablesOpen = renderTreeNodeWithIcon(
                    "Tables", tablesNodeId, ICON_FK_TABLE, ImGui::GetColorU32(colors.green));

                if (tablesOpen) {
                    if (!schemaData->tablesLoaded && !schemaData->loadingTables) {
                        // TODO: Trigger table loading for this schema
                        Logger::debug(std::format("Need to load tables for schema: {}.{}",
                                                  dbData->name, schemaData->name));
                    }

                    if (schemaData->loadingTables) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading tables...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_tables", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
                        ImGui::PopStyleColor();
                    } else if (schemaData->tablesLoaded) {
                        if (schemaData->tables.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No tables");
                            ImGui::PopStyleColor();
                        } else {
                            for (auto& table : schemaData->tables) {
                                ImGui::Text("    %s", table.name.c_str());
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

                if (viewsOpen) {
                    if (schemaData->viewsLoaded) {
                        if (schemaData->views.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No views");
                            ImGui::PopStyleColor();
                        } else {
                            for (auto& view : schemaData->views) {
                                ImGui::Text("    %s", view.name.c_str());
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }

            // Render Sequences section
            {
                const std::string seqNodeId =
                    std::format("sequences_{}_{:p}", schemaData->name,
                                static_cast<void*>(&schemaData->sequences));
                const bool seqOpen =
                    renderTreeNodeWithIcon("Sequences", seqNodeId, ICON_FK_SORT_NUMERIC_ASC,
                                           ImGui::GetColorU32(colors.mauve));

                if (seqOpen) {
                    if (schemaData->sequencesLoaded) {
                        if (schemaData->sequences.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No sequences");
                            ImGui::PopStyleColor();
                        } else {
                            for (const auto& seq : schemaData->sequences) {
                                ImGui::Text("    %s", seq.c_str());
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }

            ImGui::TreePop();
        }
    }

    void renderMySQLDatabaseNode(MySQLDatabase* mysqlDb, MySQLDatabase::DatabaseData* dbData) {
        if (!mysqlDb || !dbData) {
            return;
        }

        auto& app = Application::getInstance();
        const auto& colors = app.getCurrentColors();

        const std::string nodeId =
            std::format("db_{}_{:p}", dbData->name, static_cast<void*>(dbData));
        const bool isOpen = renderTreeNodeWithIcon(dbData->name, nodeId, ICON_FK_DATABASE,
                                                   ImGui::GetColorU32(colors.blue));

        // Handle expand/collapse
        if (ImGui::IsItemToggledOpen()) {
            dbData->expanded = isOpen;
        }

        if (isOpen) {
            // MySQL: render tables and views directly (no schema layer)

            // Render Tables section
            {
                const std::string tablesNodeId = std::format("tables_{}_{:p}", dbData->name,
                                                             static_cast<void*>(&dbData->tables));
                const bool tablesOpen = renderTreeNodeWithIcon(
                    "Tables", tablesNodeId, ICON_FK_TABLE, ImGui::GetColorU32(colors.green));

                if (tablesOpen) {
                    if (!dbData->tablesLoaded && !dbData->loadingTables) {
                        Logger::debug(
                            std::format("Need to load tables for database: {}", dbData->name));
                        // TODO: Trigger table loading for this database
                    }

                    if (dbData->loadingTables) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading tables...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_tables", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
                        ImGui::PopStyleColor();
                    } else if (dbData->tablesLoaded) {
                        if (dbData->tables.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No tables");
                            ImGui::PopStyleColor();
                        } else {
                            for (auto& table : dbData->tables) {
                                ImGui::Text("    %s", table.name.c_str());
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

                if (viewsOpen) {
                    if (!dbData->viewsLoaded && !dbData->loadingViews) {
                        Logger::debug(
                            std::format("Need to load views for database: {}", dbData->name));
                        // TODO: Trigger view loading for this database
                    }

                    if (dbData->loadingViews) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
                        ImGui::Text("  Loading views...");
                        ImGui::SameLine();
                        UIUtils::Spinner("##loading_views", 6.0f, 2,
                                         ImGui::GetColorU32(colors.peach));
                        ImGui::PopStyleColor();
                    } else if (dbData->viewsLoaded) {
                        if (dbData->views.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                            ImGui::Text("  No views");
                            ImGui::PopStyleColor();
                        } else {
                            for (auto& view : dbData->views) {
                                ImGui::Text("    %s", view.name.c_str());
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }

            ImGui::TreePop();
        }
    }

    void renderSQLiteDatabaseNode(SQLiteDatabase* sqliteDb, SQLiteDatabase::DatabaseData* dbData) {
        // TODO: Implement SQLite database node rendering using nested DatabaseData
    }

} // namespace NewHierarchy
