#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/sqlite.hpp"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// helper function to create a test database
static std::shared_ptr<DatabaseInterface> createTestDatabase(const std::string& name) {
    DatabaseConnectionInfo connInfo;
    connInfo.type = DatabaseType::SQLITE;
    connInfo.name = name;
    connInfo.path = ":memory:";

    auto db = std::make_shared<SQLiteDatabase>(connInfo);
    if (db) {
        auto [success, error] = db->connect();
        if (!success) {
            return nullptr;
        }
    }
    return db;
}

void RegisterSidebarTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    // test: sidebar visibility toggle
    t = IM_REGISTER_TEST(engine, "Sidebar", "Toggle Visibility");
    t->GuiFunc = nullptr; // main loop handles rendering
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // check sidebar is visible by default
        IM_CHECK(app.isSidebarVisible() == true);

        // toggle sidebar off
        app.setSidebarVisible(false);
        ctx->Yield(); // wait one frame

        IM_CHECK(app.isSidebarVisible() == false);

        // toggle sidebar on
        app.setSidebarVisible(true);
        ctx->Yield(); // wait one frame

        IM_CHECK(app.isSidebarVisible() == true);
    };

    // test: sidebar shows database connections
    t = IM_REGISTER_TEST(engine, "Sidebar", "Show Database List");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // ensure sidebar is visible
        app.setSidebarVisible(true);
        ctx->Yield();

        // create test database
        auto testDb = createTestDatabase("TestDB");
        IM_CHECK(testDb != nullptr);

        // add database to application
        app.addDatabase(testDb);
        ctx->Yield(2); // wait for UI to update

        // verify database appears in the list
        auto& databases = app.getDatabases();
        IM_CHECK(databases.size() >= 1);

        // cleanup
        app.removeDatabase(testDb);
        ctx->Yield();
    };

    // test: database selection
    t = IM_REGISTER_TEST(engine, "Sidebar", "Select Database");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // ensure sidebar is visible
        app.setSidebarVisible(true);
        ctx->Yield();

        // create test database
        auto testDb = createTestDatabase("SelectTestDB");
        IM_CHECK(testDb != nullptr);

        // add database to application
        app.addDatabase(testDb);
        ctx->Yield(2);

        // select database
        app.setSelectedDatabase(testDb);
        ctx->Yield();

        // verify selection
        auto selectedDb = app.getSelectedDatabase();
        IM_CHECK(selectedDb != nullptr);
        IM_CHECK(selectedDb == testDb);

        // cleanup
        app.clearSelectedDatabase();
        app.removeDatabase(testDb);
        ctx->Yield();
    };

    // test: sidebar expands/collapses tree nodes
    t = IM_REGISTER_TEST(engine, "Sidebar", "Expand Tree Nodes");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // ensure sidebar is visible
        app.setSidebarVisible(true);
        ctx->Yield();

        // create test database with some data
        auto testDb = createTestDatabase("TreeTestDB");
        IM_CHECK(testDb != nullptr);

        // connect and create a table
        if (auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(testDb)) {
            sqliteDb->executeQuery("CREATE TABLE test_table (id INTEGER PRIMARY KEY, name TEXT)");
        }

        // refresh tables (cast to SQLiteDatabase to access refreshAllTables)
        if (auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(testDb)) {
            sqliteDb->refreshAllTables();
        }

        // add database to application
        app.addDatabase(testDb);
        ctx->Yield(3); // wait for async loading

        // try to find and click the database tree node
        // note: actual ImGui ID depends on sidebar implementation
        // this is a placeholder for tree node interaction
        ctx->ItemOpen("*/Databases");
        ctx->Yield();

        // cleanup
        app.removeDatabase(testDb);
        ctx->Yield();
    };

    // test: SQLite connection dialog flow
    t = IM_REGISTER_TEST(engine, "Sidebar", "SQLite Connection Dialog");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // ensure sidebar is visible
        app.setSidebarVisible(true);
        ctx->Yield();

        // test the connection dialog workflow
        // 1. Verify we can open the connection dialog (simulated)
        // 2. Select SQLite option
        // 3. Verify path input field exists
        // 4. Verify browse button exists
        // 5. Verify connect button exists

        // note: actual button clicking would require finding the buttons
        // in the UI hierarchy, which depends on ImGui IDs
        // for now, we just verify the app doesn't crash during rendering
        ctx->Yield(5);

        IM_CHECK(true); // basic rendering stability check
    };

    // test: multiple databases in sidebar
    t = IM_REGISTER_TEST(engine, "Sidebar", "Multiple Databases");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // ensure sidebar is visible
        app.setSidebarVisible(true);
        ctx->Yield();

        // create multiple test databases
        auto db1 = createTestDatabase("Database1");
        auto db2 = createTestDatabase("Database2");
        auto db3 = createTestDatabase("Database3");

        IM_CHECK(db1 != nullptr);
        IM_CHECK(db2 != nullptr);
        IM_CHECK(db3 != nullptr);

        // add all databases
        app.addDatabase(db1);
        app.addDatabase(db2);
        app.addDatabase(db3);
        ctx->Yield(3);

        // verify all databases are in the list
        auto& databases = app.getDatabases();
        IM_CHECK(databases.size() >= 3);

        // verify we can find each database
        size_t idx1 = app.findDatabaseIndex(db1);
        size_t idx2 = app.findDatabaseIndex(db2);
        size_t idx3 = app.findDatabaseIndex(db3);

        IM_CHECK(idx1 != SIZE_MAX);
        IM_CHECK(idx2 != SIZE_MAX);
        IM_CHECK(idx3 != SIZE_MAX);

        // cleanup
        app.removeDatabase(db1);
        app.removeDatabase(db2);
        app.removeDatabase(db3);
        ctx->Yield();
    };

    // test: workspace switching
    t = IM_REGISTER_TEST(engine, "Sidebar", "Workspace Switching");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // get current workspace
        int currentWorkspace = app.getCurrentWorkspaceId();
        IM_CHECK(currentWorkspace >= 1);

        // create new workspace
        int newWorkspaceId = app.createWorkspace("Test Workspace", "Test workspace for UI testing");
        IM_CHECK(newWorkspaceId > 0);
        ctx->Yield();

        // switch to new workspace
        app.setCurrentWorkspace(newWorkspaceId);
        ctx->Yield(2);

        // verify workspace changed
        IM_CHECK(app.getCurrentWorkspaceId() == newWorkspaceId);

        // switch back to original workspace
        app.setCurrentWorkspace(currentWorkspace);
        ctx->Yield();

        // cleanup - delete test workspace
        app.deleteWorkspace(newWorkspaceId);
        ctx->Yield();
    };

    // test: sidebar renders without crashing
    t = IM_REGISTER_TEST(engine, "Sidebar", "Basic Rendering");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();

        // ensure sidebar is visible
        app.setSidebarVisible(true);

        // render several frames
        for (int i = 0; i < 10; i++) {
            ctx->Yield();
        }

        // if we got here without crashing, test passes
        IM_CHECK(true);
    };
}
