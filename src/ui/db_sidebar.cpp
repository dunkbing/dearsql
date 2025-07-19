#include "ui/db_sidebar.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "imgui.h"
#include "tabs/tab_manager.hpp"
#include "utils/spinner.hpp"
#include <iostream>

void DatabaseSidebar::showConnectionDialog() {
    shouldShowConnectionDialog = true;
}

void DatabaseSidebar::render() {
    auto &app = Application::getInstance();

    ImGui::Begin("Databases", nullptr, ImGuiWindowFlags_NoScrollbar);

    // Check if we should show the connection dialog
    if (shouldShowConnectionDialog) {
        connectionDialog.showDialog();
        shouldShowConnectionDialog = false;
    }

    // Always render the dialog to handle multi-frame interactions
    if (connectionDialog.isDialogOpen()) {
        connectionDialog.showDialog();
    }

    // Check if dialog completed and get result
    if (const auto db = connectionDialog.getResult()) {
        auto [success, error] = db->connect();
        if (success) {
            // Only refresh tables immediately for SQLite, PostgreSQL will do it async when needed
            if (db->getType() == DatabaseType::SQLITE) {
                db->refreshTables();
            }
            std::cout << "Adding database to list." << std::endl;
            app.addDatabase(db);
        } else {
            std::cerr << "Failed to open database: " << error << std::endl;
        }
    }

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 6.0f));
    auto &databases = app.getDatabases();

    if (databases.empty()) {
        // Show helpful message when no databases are connected
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextWrapped("No databases connected");
        ImGui::Spacing();
        ImGui::TextWrapped("Right-click here to add a new database connection");
        ImGui::PopStyleColor();

        // Show context menu for adding database when area is right-clicked
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("AddDatabasePopup");
        }

        if (ImGui::BeginPopup("AddDatabasePopup")) {
            if (ImGui::MenuItem("Add Database Connection")) {
                showConnectionDialog();
            }
            ImGui::EndPopup();
        }
    } else {
        for (size_t i = 0; i < databases.size(); i++) {
            renderDatabaseNode(i);
        }
    }

    ImGui::PopStyleVar();

    // Handle delete confirmation dialog
    if (shouldShowDeleteConfirmation) {
        ImGui::OpenPopup("Confirm Delete Database");
        shouldShowDeleteConfirmation = false;
    }

    if (ImGui::BeginPopupModal("Confirm Delete Database", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        auto &databases = app.getDatabases();
        if (databaseToDelete < databases.size()) {
            auto &db = databases[databaseToDelete];
            ImGui::Text("Are you sure you want to remove this database connection?");
            ImGui::Text("Database: %s", db->getName().c_str());
            ImGui::Spacing();
            ImGui::Text("This will:");
            ImGui::BulletText("Remove the database from the current session");
            ImGui::BulletText("Delete the saved connection (if any)");
            ImGui::BulletText("Close any open tabs for this database");
            ImGui::Spacing();
            ImGui::Separator();

            if (ImGui::Button("Remove", ImVec2(100, 0))) {
                // Remove from saved connections by finding matching connection
                auto savedConnections = app.getAppState()->getSavedConnections();
                for (const auto &conn : savedConnections) {
                    bool matches = false;
                    if (db->getType() == DatabaseType::POSTGRESQL && conn.type == "postgresql") {
                        matches = (conn.name == db->getName());
                    } else if (db->getType() == DatabaseType::SQLITE && conn.type == "sqlite") {
                        matches = (conn.path == db->getPath());
                    }

                    if (matches) {
                        if (app.getAppState()->deleteConnection(conn.id)) {
                            std::cout << "deleted " << conn.id << std::endl;
                        }
                        break;
                    }
                }

                // Remove from application
                app.removeDatabase(databaseToDelete);

                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void DatabaseSidebar::renderDatabaseNode(const size_t databaseIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    // Database node
    ImGuiTreeNodeFlags dbFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_FramePadding;
    if (app.getSelectedDatabase() == static_cast<int>(databaseIndex)) {
        dbFlags |= ImGuiTreeNodeFlags_Selected;
    }

    // Show loading indicator in database name if connecting
    const std::string dbIcon =
        (db->getType() == DatabaseType::SQLITE) ? ICON_FA_DATABASE : ICON_FA_TOWER_BROADCAST;
    const std::string dbLabel = std::format("{} {}", dbIcon, db->getName());
    const bool showSpinner = db->isConnecting();

    const bool dbOpen = ImGui::TreeNodeEx(dbLabel.c_str(), dbFlags);

    // Show spinner next to database name if connecting
    if (showSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##db_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(static_cast<int>(databaseIndex));
        app.setSelectedTable(-1);
    }

    // Context menu for database
    handleDatabaseContextMenu(databaseIndex);

    // Check for async connection completion (always check, even when collapsed)
    db->checkConnectionStatusAsync();

    if (dbOpen) {
        // Auto-connect when database node is expanded
        if (!db->isConnected() && !db->hasAttemptedConnection() && !db->isConnecting()) {
            std::cout << "Starting async connection to database: " << db->getName() << std::endl;
            db->startConnectionAsync();
        }

        // Check if database is connected
        if (db->isConnecting()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("  Connecting...");
            ImGui::SameLine();
            UIUtils::Spinner("##connecting_spinner", 6.0f, 2,
                             ImGui::GetColorU32(ImVec4(1.0f, 0.7f, 0.3f, 1.0f)));
            ImGui::PopStyleColor();
        } else if (!db->isConnected() && !db->hasAttemptedConnection()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::Text("  Click to connect");
            ImGui::PopStyleColor();
        } else if (db->hasAttemptedConnection() && !db->isConnected() &&
                   !db->getLastConnectionError().empty()) {
            // Show connection error
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("  Connection failed: %s", db->getLastConnectionError().c_str());
            ImGui::PopStyleColor();
        } else if (db->isConnected()) {
            // Check for async loading completion for PostgreSQL
            if (db->getType() == DatabaseType::POSTGRESQL) {
                if (db->isLoadingTables()) {
                    db->checkTablesStatusAsync();
                }
                if (db->isLoadingViews()) {
                    db->checkViewsStatusAsync();
                }
                if (db->isLoadingSequences()) {
                    db->checkSequencesStatusAsync();
                }
            }

            // Show hierarchical structure based on database type
            if (db->getType() == DatabaseType::SQLITE) {
                renderSQLiteHierarchy(databaseIndex);
            } else if (db->getType() == DatabaseType::POSTGRESQL) {
                renderPostgresHierarchy(databaseIndex);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::renderTableNode(const int databaseIndex, const int tableIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    const auto &table = db->getTables()[tableIndex];

    ImGuiTreeNodeFlags tableFlags =
        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;
    if (app.getSelectedDatabase() == databaseIndex && app.getSelectedTable() == tableIndex) {
        tableFlags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string tableLabel = std::format("{} {}", ICON_FA_TABLE, table.name);
    const bool tableOpened = ImGui::TreeNodeEx(tableLabel.c_str(), tableFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(databaseIndex);
        app.setSelectedTable(tableIndex);
    }

    // Double-click to open table viewer (async loading will be handled by the tab)
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
    }

    handleTableContextMenu(databaseIndex, tableIndex);

    if (tableOpened) {
        // Columns section
        constexpr ImGuiTreeNodeFlags columnsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                    ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                    ImGuiTreeNodeFlags_FramePadding;
        const std::string columnsLabel = std::format("{} Columns", ICON_FA_TABLE_COLUMNS);
        const bool columnsOpened = ImGui::TreeNodeEx(columnsLabel.c_str(), columnsFlags);

        if (columnsOpened) {
            for (const auto &[name, type, isPrimaryKey, isNotNull] : table.columns) {
                ImGuiTreeNodeFlags columnFlags = ImGuiTreeNodeFlags_Leaf |
                                                 ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_FramePadding;

                // Build column display string with type and constraints
                std::string columnDisplay = std::format("{} ({}", name, type);
                if (isPrimaryKey) {
                    columnDisplay += ", PK";
                }
                if (isNotNull) {
                    columnDisplay += ", NOT NULL";
                }
                columnDisplay += ")";

                ImGui::TreeNodeEx(columnDisplay.c_str(), columnFlags);
            }
            ImGui::TreePop();
        }

        // Keys section
        constexpr ImGuiTreeNodeFlags keysFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                 ImGuiTreeNodeFlags_FramePadding;
        const std::string keysLabel = std::format("{} Keys", ICON_FA_KEY);
        bool keysOpen = ImGui::TreeNodeEx(keysLabel.c_str(), keysFlags);

        if (keysOpen) {
            // Show primary key if any column is marked as primary key
            bool hasPrimaryKey = false;
            std::string primaryKeyColumns;
            for (const auto &column : table.columns) {
                if (column.isPrimaryKey) {
                    if (hasPrimaryKey) {
                        primaryKeyColumns += ", ";
                    }
                    primaryKeyColumns += column.name;
                    hasPrimaryKey = true;
                }
            }

            if (hasPrimaryKey) {
                constexpr ImGuiTreeNodeFlags pkFlags = ImGuiTreeNodeFlags_Leaf |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                       ImGuiTreeNodeFlags_FramePadding;
                std::string pkDisplay = "Primary Key (" + primaryKeyColumns + ")";
                ImGui::TreeNodeEx(pkDisplay.c_str(), pkFlags);
            } else {
                ImGui::Text("  No primary key");
            }
            ImGui::TreePop();
        }

        // Indexes section
        constexpr ImGuiTreeNodeFlags indexesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                    ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                    ImGuiTreeNodeFlags_FramePadding;
        const std::string indexesLabel = std::format("{} Indexes", ICON_FA_MAGNIFYING_GLASS);
        bool indexesOpen = ImGui::TreeNodeEx(indexesLabel.c_str(), indexesFlags);

        if (indexesOpen) {
            // TODO: Implement index retrieval from database
            ImGui::Text("  Index information not available");
            ImGui::TreePop();
        }

        ImGui::TreePop();
    }
}

void DatabaseSidebar::handleDatabaseContextMenu(size_t databaseIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Refresh All")) {
            db->setTablesLoaded(false);
            db->setViewsLoaded(false);
            if (db->getType() == DatabaseType::POSTGRESQL) {
                db->setSequencesLoaded(false);
            }
            db->refreshTables();
            db->refreshViews();
            if (db->getType() == DatabaseType::POSTGRESQL) {
                db->refreshSequences();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Refresh Tables")) {
            db->setTablesLoaded(false);
            db->refreshTables();
        }
        if (ImGui::MenuItem("Refresh Views")) {
            db->setViewsLoaded(false);
            db->refreshViews();
        }
        if (db->getType() == DatabaseType::POSTGRESQL && ImGui::MenuItem("Refresh Sequences")) {
            db->setSequencesLoaded(false);
            db->refreshSequences();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Retry Connection") && db->hasAttemptedConnection() &&
            !db->isConnected()) {
            // Reset connection attempt state to allow retry
            db->setAttemptedConnection(false);
            db->setTablesLoaded(false);
            db->setViewsLoaded(false);
            db->setSequencesLoaded(false);
            db->setLastConnectionError("");
        }
        if (ImGui::MenuItem("New SQL Editor")) {
            app.getTabManager()->createSQLEditorTab();
        }
        if (ImGui::MenuItem("Disconnect")) {
            db->disconnect();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Database")) {
            shouldShowDeleteConfirmation = true;
            databaseToDelete = databaseIndex;
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::handleTableContextMenu(const size_t databaseIndex, const size_t tableIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &table = db->getTables()[tableIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), table.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show table structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::renderViewNode(size_t databaseIndex, size_t viewIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &view = db->getViews()[viewIndex];

    ImGuiTreeNodeFlags viewFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_FramePadding;

    const std::string viewLabel = std::format("{} {}", ICON_FA_EYE, view.name);
    ImGui::TreeNodeEx(viewLabel.c_str(), viewFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(databaseIndex);
        app.setSelectedTable(-1); // Reset table selection for views
    }

    // Double-click to open view viewer (async loading will be handled by the tab)
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        app.getTabManager()->createTableViewerTab(db->getConnectionString(), view.name);
    }

    handleViewContextMenu(databaseIndex, viewIndex);
}

void DatabaseSidebar::renderSequenceNode(size_t databaseIndex, size_t sequenceIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &sequence = db->getSequences()[sequenceIndex];

    ImGuiTreeNodeFlags sequenceFlags = ImGuiTreeNodeFlags_Leaf |
                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_FramePadding;

    const std::string sequenceLabel = std::format("{} {}", ICON_FA_LIST_OL, sequence);
    ImGui::TreeNodeEx(sequenceLabel.c_str(), sequenceFlags);

    if (ImGui::IsItemClicked()) {
        app.setSelectedDatabase(databaseIndex);
        app.setSelectedTable(-1); // Reset table selection for sequences
    }

    handleSequenceContextMenu(databaseIndex, sequenceIndex);
}

void DatabaseSidebar::renderSQLiteHierarchy(size_t databaseIndex) {
    renderTablesSection(databaseIndex);
    renderViewsSection(databaseIndex);
}

void DatabaseSidebar::renderPostgresHierarchy(size_t databaseIndex) {
    renderTablesSection(databaseIndex);
    renderViewsSection(databaseIndex);
    renderSequencesSection(databaseIndex);
}

void DatabaseSidebar::renderTablesSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    ImGuiTreeNodeFlags tablesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                     ImGuiTreeNodeFlags_FramePadding;

    // Show loading indicator next to Tables node if loading
    std::string tablesLabel = std::format("{} Tables", ICON_FA_TABLE);
    bool showTablesSpinner = (db->getType() == DatabaseType::POSTGRESQL && db->isLoadingTables());

    bool tablesOpen = ImGui::TreeNodeEx(tablesLabel.c_str(), tablesFlags);

    // Show spinner next to Tables node if loading
    if (showTablesSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##tables_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    // Load tables when the tree node is opened and tables haven't been loaded yet
    if (tablesOpen && !db->areTablesLoaded() && !db->isLoadingTables()) {
        std::cout << "Tables node expanded and tables not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshTables();
    }

    if (tablesOpen) {
        if (db->getTables().empty()) {
            if (db->isLoadingTables()) {
                // Show loading indicator with spinner
                ImGui::Text("  Loading tables...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_tables_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            } else if (!db->areTablesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No tables found");
            }
        } else {
            for (size_t j = 0; j < db->getTables().size(); j++) {
                renderTableNode(databaseIndex, j);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::renderViewsSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    ImGuiTreeNodeFlags viewsFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                    ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                    ImGuiTreeNodeFlags_FramePadding;

    // Show loading indicator next to Views node if loading
    std::string viewsLabel = std::format("{} Views", ICON_FA_EYE);
    bool showViewsSpinner = (db->getType() == DatabaseType::POSTGRESQL && db->isLoadingViews());

    bool viewsOpen = ImGui::TreeNodeEx(viewsLabel.c_str(), viewsFlags);

    // Show spinner next to Views node if loading
    if (showViewsSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##views_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    // Load views when the tree node is opened and views haven't been loaded yet
    if (viewsOpen && !db->areViewsLoaded() && !db->isLoadingViews()) {
        std::cout << "Views node expanded and views not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshViews();
    }

    if (viewsOpen) {
        if (db->getViews().empty()) {
            if (db->isLoadingViews()) {
                // Show loading indicator with spinner
                ImGui::Text("  Loading views...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_views_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            } else if (!db->areViewsLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No views found");
            }
        } else {
            for (size_t j = 0; j < db->getViews().size(); j++) {
                renderViewNode(databaseIndex, j);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::renderSequencesSection(size_t databaseIndex) {
    auto &app = Application::getInstance();
    auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];

    // Only show sequences for PostgreSQL
    if (db->getType() != DatabaseType::POSTGRESQL) {
        return;
    }

    ImGuiTreeNodeFlags sequencesFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                        ImGuiTreeNodeFlags_FramePadding;

    // Show loading indicator next to Sequences node if loading
    std::string sequencesLabel = std::format("{} Sequences", ICON_FA_LIST_OL);
    bool showSequencesSpinner =
        (db->getType() == DatabaseType::POSTGRESQL && db->isLoadingSequences());

    bool sequencesOpen = ImGui::TreeNodeEx(sequencesLabel.c_str(), sequencesFlags);

    // Show spinner next to Sequences node if loading
    if (showSequencesSpinner) {
        ImGui::SameLine();
        UIUtils::Spinner("##sequences_spinner", 6.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    }

    // Load sequences when the tree node is opened and sequences haven't been loaded yet
    if (sequencesOpen && !db->areSequencesLoaded() && !db->isLoadingSequences()) {
        std::cout << "Sequences node expanded and sequences not loaded yet, attempting to load..."
                  << std::endl;
        db->refreshSequences();
    }

    if (sequencesOpen) {
        if (db->getSequences().empty()) {
            if (db->isLoadingSequences()) {
                // Show loading indicator with spinner
                ImGui::Text("  Loading sequences...");
                ImGui::SameLine();
                UIUtils::Spinner("##loading_sequences_spinner", 6.0f, 2,
                                 ImGui::GetColorU32(ImGuiCol_Text));
            } else if (!db->areSequencesLoaded()) {
                ImGui::Text("  Loading...");
            } else {
                ImGui::Text("  No sequences found");
            }
        } else {
            for (size_t j = 0; j < db->getSequences().size(); j++) {
                renderSequenceNode(databaseIndex, j);
            }
        }
        ImGui::TreePop();
    }
}

void DatabaseSidebar::handleViewContextMenu(const size_t databaseIndex, const size_t viewIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    const auto &view = db->getViews()[viewIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("View Data")) {
            app.getTabManager()->createTableViewerTab(db->getConnectionString(), view.name);
        }
        if (ImGui::MenuItem("Show Structure")) {
            // TODO: Show view structure in a tab
        }
        ImGui::EndPopup();
    }
}

void DatabaseSidebar::handleSequenceContextMenu(const size_t databaseIndex,
                                                const size_t sequenceIndex) {
    auto &app = Application::getInstance();
    const auto &databases = app.getDatabases();
    auto &db = databases[databaseIndex];
    auto &sequence = db->getSequences()[sequenceIndex];

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Show Details")) {
            // TODO: Show sequence details in a tab
        }
        ImGui::EndPopup();
    }
}
