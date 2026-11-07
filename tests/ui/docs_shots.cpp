#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "database/s3.hpp"
#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "platform/graphics_backend.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab_manager.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

// Not tests: these drive the app into a state and hold it there, so a screenshot
// can be taken of a real window. Run one at a time, e.g.
//
//   ./build/ui_tests -nopause -run "Docs/assistant-empty"
//
// SleepNoSkip is used rather than Sleep because Sleep is skipped in Fast mode.
namespace {
    // Hold by the wall clock, not ctx->Sleep*: those count simulation time, and in
    // Fast mode the engine burns through it in a fraction of a real second.
    void holdForScreenshot(ImGuiTestContext* ctx, int seconds = 20) {
        // DEARSQL_SHOT_DIR: render offscreen at 2x into <dir>/<name>.png, crisp on any display
        if (const char* dir = std::getenv("DEARSQL_SHOT_DIR")) {
            ctx->Yield(30); // let hover and popups settle
            requestFrameCapture(std::format("{}/{}.png", dir, ctx->Test->Name).c_str(), 2);
            while (frameCapturePending()) {
                ctx->Yield();
            }
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < until) {
            ctx->Yield();
        }
    }

    // a chat tab on the first saved connection, as its context menu would open it
    void openAssistant(ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.setSidebarVisible(true);
        const auto& dbs = app.getDatabases();
        const auto tab = app.getTabManager()->createAIChatTab(dbs.empty() ? nullptr : dbs.front());
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
    }

    // the context picker has nothing to list until a connection is open, and
    // connections are opened lazily by expanding them in the sidebar
    void connectEverything(ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        for (const auto& db : app.getDatabases()) {
            if (db && !db->isConnected()) {
                db->startConnectionAsync();
            }
        }
        for (int i = 0; i < 400; ++i) {
            bool anyConnected = false;
            for (const auto& db : app.getDatabases()) {
                if (!db) {
                    continue;
                }
                db->checkConnectionStatusAsync();
                anyConnected = anyConnected || db->isConnected();
            }
            ctx->Yield();
            if (anyConnected && i > 60) {
                break; // connected, plus a moment for schema to load
            }
        }
    }

    // first open connection that is its own node (sqlite/duckdb) and has tables
    std::shared_ptr<DatabaseInterface> firstDbWithTables(ImGuiTestContext* ctx) {
        for (const auto& db : Application::getInstance().getDatabases()) {
            auto* node = db ? dynamic_cast<IDatabaseNode*>(db.get()) : nullptr;
            if (node == nullptr || !db->isConnected()) {
                continue;
            }
            node->startTablesLoadAsync(true);
            for (int i = 0; i < 300 && !node->isTablesLoaded(); ++i) {
                node->checkLoadingStatus();
                ctx->Yield();
            }
            // a csv opens as a one-table connection; prefer a real database
            if (node->getTables().size() > 1) {
                return db;
            }
        }
        return nullptr;
    }

    // tree node ids are built from pointers (db_sidebar/database_node), so the
    // label alone can't address them -- rebuild the same ids here
    void expandInSidebar(ImGuiTestContext* ctx, const std::shared_ptr<DatabaseInterface>& db) {
        ctx->SetRef("Databases");
        ctx->ItemOpen(std::format("**/###db_{:p}", static_cast<const void*>(db.get())).c_str());
        ctx->Yield(5);
        ctx->ItemOpen(std::format("**/###sqlite_tables_{:p}",
                                  static_cast<void*>(dynamic_cast<FileDatabase*>(db.get())))
                          .c_str());
        ctx->Yield(5);
    }
} // namespace

void RegisterDocsShots(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    // the panel as it looks on opening, before anything is typed
    t = IM_REGISTER_TEST(engine, "Docs", "assistant-empty");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistant(ctx);
        holdForScreenshot(ctx);
    };

    // the @ picker listing databases, tables and views
    t = IM_REGISTER_TEST(engine, "Docs", "assistant-context-picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistant(ctx);
        connectEverything(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("@");
        ctx->Yield(120); // let the picker pull in tables as they load
        holdForScreenshot(ctx);
    };

    // the / command list, DearSQL's own plus whatever the agent published
    t = IM_REGISTER_TEST(engine, "Docs", "assistant-commands");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistant(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("/");
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };

    // the sidebar tree, for the browsing-data page
    t = IM_REGISTER_TEST(engine, "Docs", "sidebar-tree");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        Application::getInstance().setSidebarVisible(true);
        ctx->Yield(2);
        ctx->SetRef("Databases");
        connectEverything(ctx);
        if (const auto db = firstDbWithTables(ctx)) {
            expandInSidebar(ctx, db);
        }
        holdForScreenshot(ctx);
    };

    // the connection dialog, for the connections page
    t = IM_REGISTER_TEST(engine, "Docs", "connection-dialog");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ConnectionDialog::instance().show(&Application::getInstance());
        ctx->Yield(5);
        // postgres rather than the sqlite default: host, ssl and ssh all show
        ctx->SetRef("//$FOCUSED"); // the modal's title carries a ### id suffix
        ctx->ComboClick("##conn_type/PostgreSQL");
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };

    // a table open in the grid, for the browsing page
    t = IM_REGISTER_TEST(engine, "Docs", "table-grid");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        auto* node = dynamic_cast<IDatabaseNode*>(db.get());
        // DEARSQL_SHOT_TABLE picks a wider table (the readme hero)
        const auto& tables = node->getTables();
        const char* want = std::getenv("DEARSQL_SHOT_TABLE");
        const auto it =
            std::ranges::find_if(tables, [&](const auto& tbl) { return want && tbl.name == want; });
        Application::getInstance().getTabManager()->createTableViewerTab(
            node, it != tables.end() ? *it : tables.front());
        expandInSidebar(ctx, db);
        ctx->Yield(120); // let the first page of rows load
        holdForScreenshot(ctx);
    };

    // an editor holding a query and its results
    t = IM_REGISTER_TEST(engine, "Docs", "sql-editor");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        auto* node = dynamic_cast<IDatabaseNode*>(db.get());
        const auto tab = Application::getInstance().getTabManager()->createSQLEditorTab("", node);
        const auto editor = std::dynamic_pointer_cast<SQLEditorTab>(tab);
        editor->setQuery(
            std::format("select *\nfrom {}\nlimit 50;", node->getTables().front().name));
        expandInSidebar(ctx, db);
        ctx->Yield(10);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/" ICON_FA_PLAY " Run");
        ctx->Yield(120);
        holdForScreenshot(ctx);
    };

    // a table's context menu, where import/export lives
    t = IM_REGISTER_TEST(engine, "Docs", "table-menu");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        expandInSidebar(ctx, db);
        const auto& table = dynamic_cast<IDatabaseNode*>(db.get())->getTables().front();
        ctx->ItemClick(
            std::format("**/###sqlite_table_{}_{:p}", table.name, static_cast<const void*>(&table))
                .c_str(),
            ImGuiMouseButton_Right);
        ctx->Yield(5);
        ctx->SetRef("//$FOCUSED");
        // hover, not click: clicking refocuses the parent popup, which then draws
        // over the submenu it just opened
        ctx->MouseMove("Export");
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };

    // the csv editor with a cell selected
    t = IM_REGISTER_TEST(engine, "Docs", "csv-editor");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const auto path = std::filesystem::temp_directory_path() / "employees.csv";
        std::ofstream(path, std::ios::binary)
            << "id;name;department;city;salary;hired\n"
               "1;Ada Lovelace;Engineering;London;98000;2019-03-04\n"
               "2;Grace Hopper;Engineering;New York;105000;2017-08-21\n"
               "3;Linus Pauling;Research;Portland;87000;2020-01-13\n"
               "4;Margaret Hamilton;Engineering;Boston;112000;2016-05-30\n"
               "5;Alan Turing;Research;Manchester;91000;2018-11-02\n"
               "6;Katherine Johnson;Operations;Hampton;79000;2021-06-15\n"
               "7;Dennis Ritchie;Engineering;Murray Hill;101000;2015-09-09\n"
               "8;Barbara Liskov;Research;Cambridge;99000;2019-12-01\n"
               "9;Ken Thompson;Operations;Murray Hill;;2022-02-28\n"
               "10;Frances Allen;Engineering;Yorktown;97000;2020-10-19\n";
        const auto tab =
            Application::getInstance().getTabManager()->createCsvEditorTab(path.string());
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        for (int i = 0; i < 300 && !ctx->ItemExists("**/Alan Turing"); ++i)
            ctx->Yield();
        ctx->ItemClick("**/Grace Hopper");
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };

    // an s3 bucket in the browser tab; needs DEARSQL_TEST_S3_* (scripts/run-tests starts one)
    t = IM_REGISTER_TEST(engine, "Docs", "s3-browser");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* endpoint = std::getenv("DEARSQL_TEST_S3_ENDPOINT");
        const char* access = std::getenv("DEARSQL_TEST_S3_ACCESS_KEY");
        const char* secret = std::getenv("DEARSQL_TEST_S3_SECRET_KEY");
        IM_CHECK_NO_RET(endpoint && access && secret);
        if (!endpoint || !access || !secret)
            return;
        DatabaseConnectionInfo info;
        info.type = DatabaseType::S3;
        info.name = "Object Storage";
        const std::string ep = endpoint;
        info.host = ep.substr(0, ep.rfind(':'));
        info.port = std::stoi(ep.substr(ep.rfind(':') + 1));
        info.sslmode = SslMode::Disable;
        info.username = access;
        info.password = secret;
        auto db = std::dynamic_pointer_cast<S3Database>(DatabaseFactory::createDatabase(info));
        IM_CHECK_NO_RET(db && db->connect().first);

        // a bucket with a few folders and files
        const auto client = db->client();
        const std::string bucket = "analytics-exports";
        try {
            client->deletePrefix(bucket, "");
            client->deleteBucket(bucket);
        } catch (const std::exception&) {
        }
        client->createBucket(bucket);
        const auto tmp = std::filesystem::temp_directory_path() / "dearsql_s3_shot.bin";
        for (const auto& [key, size] :
             std::vector<std::pair<std::string, int>>{{"2026/05/orders.parquet", 1},
                                                      {"2026/06/orders.parquet", 1},
                                                      {"archive/2025-orders.csv.gz", 1},
                                                      {"customers.csv", 48213},
                                                      {"orders-2026-06.parquet", 2817446},
                                                      {"products.json", 9182},
                                                      {"README.md", 1204}}) {
            std::ofstream(tmp, std::ios::binary) << std::string(size, 'x');
            client->upload(bucket, key, tmp.string());
        }
        std::filesystem::remove(tmp);

        Application::getInstance().addDatabase(db);
        const auto tab = Application::getInstance().getTabManager()->createS3BrowserTab(db, bucket);
        ctx->SetRef(tab->getWindowName().c_str());
        for (int i = 0; i < 300 && !ctx->ItemExists("**/" ICON_FA_FILE " customers.csv"); ++i)
            ctx->Yield();
        ctx->ItemClick("**/" ICON_FA_FILE " customers.csv");
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };

    // the settings dialog, with the command-line install row
    t = IM_REGISTER_TEST(engine, "Docs", "app-settings");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SettingsDialog::instance().open();
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };

    // the AI settings dialog, for the database-tools page
    t = IM_REGISTER_TEST(engine, "Docs", "ai-settings");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistant(ctx);
        AISettingsDialog::instance().show();
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };
}
