#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "database/postgresql.hpp"
#include "database/s3.hpp"
#include "frame_capture.hpp"
#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "mydear/app.hpp"
#include "ui/ai_settings_dialog.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/schema_compare.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/tab/s3_browser_tab.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/table_viewer_tab.hpp"
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
            frame_capture::requestPng(std::format("{}/{}.png", dir, ctx->Test->Name), 2);
            while (frame_capture::pending()) {
                ctx->Yield();
            }
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < until) {
            ctx->Yield();
        }
    }

    // customers and orders: orders.customer_id unindexed, orders_status a prefix of
    // orders_status_total. added to the app, tables loaded
    std::shared_ptr<FileDatabase> makeShopDatabase(const char* name) {
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = name;
        info.path = ":memory:";
        auto db = std::make_shared<FileDatabase>(info);
        if (!db->connect().first)
            return nullptr;
        for (const char* sql :
             {"CREATE TABLE customers (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, "
              "name TEXT)",
              "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER NOT NULL "
              "REFERENCES customers(id), status TEXT NOT NULL, total REAL, created_at TEXT)",
              "CREATE INDEX orders_status ON orders (status)",
              "CREATE INDEX orders_status_total ON orders (status, total)"})
            db->executeQuery(sql);
        Application::getInstance().addDatabase(db);
        return db;
    }

    void loadTables(ImGuiTestContext* ctx, FileDatabase& db) {
        db.startTablesLoadAsync(true);
        for (int i = 0; i < 300 && !db.isTablesLoaded(); ++i) {
            db.checkLoadingStatus();
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
        ctx->ItemClick("**/Run");
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
        const auto s3tab = std::static_pointer_cast<S3BrowserTab>(tab);
        // drag from one row to another box-selects the rows between
        ctx->MouseMove("**/" ICON_FA_FILE " README.md");
        ctx->MouseDown();
        ctx->MouseMove("**/" ICON_FA_FILE " orders-2026-06.parquet",
                       ImGuiTestOpFlags_NoCheckHoveredId);
        ctx->MouseUp();
        IM_CHECK_EQ(s3tab->selectedCount(), 3u);
        ctx->ItemClick("**/" ICON_FA_FILE " customers.csv");
        IM_CHECK_EQ(s3tab->selectedCount(), 1u);
        ctx->KeyDown(ImGuiMod_Ctrl); // a second file joins the selection
        ctx->ItemClick("**/" ICON_FA_FILE " products.json");
        ctx->KeyUp(ImGuiMod_Ctrl);
        ctx->Yield(5);
        IM_CHECK_EQ(s3tab->selectedCount(), 2u);
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

    // explain analyze on postgres; needs the DEARSQL_TEST_PG_* server scripts/run-tests starts
    t = IM_REGISTER_TEST(engine, "Docs", "sql-explain");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* host = std::getenv("DEARSQL_TEST_PG_HOST");
        if (!host) {
            ctx->LogError("set DEARSQL_TEST_PG_HOST/_PORT/_DB/_USER/_PASSWORD");
            return;
        }
        auto env = [](const char* name, const char* fallback) {
            const char* v = std::getenv(name);
            return std::string(v ? v : fallback);
        };
        DatabaseConnectionInfo info;
        info.name = "shop";
        info.type = DatabaseType::POSTGRESQL;
        info.host = host;
        info.port = std::stoi(env("DEARSQL_TEST_PG_PORT", "5432"));
        info.database = env("DEARSQL_TEST_PG_DB", "postgres");
        info.username = env("DEARSQL_TEST_PG_USER", "postgres");
        info.password = env("DEARSQL_TEST_PG_PASSWORD", "");
        auto db = std::make_shared<PostgresDatabase>(info);
        if (auto [ok, err] = db->connect(); !ok) {
            ctx->LogError("%s", err.c_str());
            return;
        }
        auto& app = Application::getInstance();
        app.addDatabase(db);
        db->executeQuery(
            "DROP TABLE IF EXISTS orders, customers;"
            "CREATE TABLE customers (id int PRIMARY KEY, name text, country text);"
            "CREATE TABLE orders (id int PRIMARY KEY, customer_id int REFERENCES customers, "
            "total numeric, created_at timestamptz);"
            "INSERT INTO customers SELECT g, 'customer ' || g, (ARRAY['US','DE','VN','FR','JP'])[1 "
            "+ "
            "g % 5] FROM generate_series(1, 5000) g;"
            "INSERT INTO orders SELECT g, 1 + g % 5000, (g % 500) / 3.0, now() - (g % 365) * "
            "interval '1 day' FROM generate_series(1, 200000) g;"
            "ANALYZE customers; ANALYZE orders;");
        auto* node = db->getDatabaseData(info.database);
        const auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
            app.getTabManager()->createSQLEditorTab("", node));
        tab->setQuery("select c.country, count(*), sum(o.total)\nfrom orders o\njoin customers c "
                      "on c.id = o.customer_id\nwhere o.created_at > now() - interval '30 "
                      "days'\ngroup by c.country\norder by 3 desc;");
        ctx->Yield(10);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/###explain_analyze");
        ctx->ItemClick("**/Explain");
        // park the mouse over the sql text: no tooltip over the plan
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ctx->MouseMoveToPos(ImVec2(vp->Pos.x + vp->Size.x * 0.6f, vp->Pos.y + 200.0f));
        for (int i = 0; i < 600 && !tab->getPlan(); ++i) {
            ctx->Yield();
        }
        holdForScreenshot(ctx);
        db->executeQuery("DROP TABLE IF EXISTS orders, customers");
        app.getTabManager()->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
    };

    // a script with the last statement's play icon hovered: the outline shows what runs
    t = IM_REGISTER_TEST(engine, "Docs", "sql-run-statement");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        auto* node = dynamic_cast<IDatabaseNode*>(db.get());
        const auto& tables = node->getTables();
        const auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
            Application::getInstance().getTabManager()->createSQLEditorTab("", node));
        tab->setQuery(std::format("select *\nfrom {}\nlimit 20;\n\nselect count(*)\nfrom {};\n\n"
                                  "select *\nfrom {}\nlimit 5;",
                                  tables[0].name, tables[1 % tables.size()].name,
                                  tables[2 % tables.size()].name));
        expandInSidebar(ctx, db);
        ctx->Yield(10);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/##run_2");
        ctx->Yield(60);
        ctx->MouseMove("**/##run_2");
        holdForScreenshot(ctx);
    };

    // a typo underlined where the parser stopped, the message next to the toolbar
    t = IM_REGISTER_TEST(engine, "Docs", "sql-syntax-error");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        auto* node = dynamic_cast<IDatabaseNode*>(db.get());
        const auto& tables = node->getTables();
        const auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
            Application::getInstance().getTabManager()->createSQLEditorTab("", node));
        tab->setQuery(std::format("select count(*)\nfrom {};\n\nselect *\nfrm {}\nlimit 10;",
                                  tables[0].name, tables[1 % tables.size()].name));
        expandInSidebar(ctx, db);
        for (int i = 0; i < 120 && tab->getEditor().GetErrorOffset() < 0; ++i) {
            ctx->Yield();
        }
        ctx->Yield(10);
        holdForScreenshot(ctx);
    };

    // :name placeholders prompt for their values before the query runs
    t = IM_REGISTER_TEST(engine, "Docs", "sql-parameters");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        auto* node = dynamic_cast<IDatabaseNode*>(db.get());
        const auto& tables = node->getTables();
        const auto tab = std::dynamic_pointer_cast<SQLEditorTab>(
            Application::getInstance().getTabManager()->createSQLEditorTab("", node));
        tab->setQuery(std::format("select *\nfrom {}\nwhere rowid >= :from_id\n  and rowid < "
                                  ":from_id + :page_size;",
                                  tables[0].name));
        expandInSidebar(ctx, db);
        ctx->Yield(10);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/Run");
        ctx->Yield(3);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemInputValue("##param_from_id", "100");
        ctx->KeyChars("25");
        ctx->Yield(10);
        holdForScreenshot(ctx);
    };

    // cmd/ctrl+p over the loaded tables and views
    t = IM_REGISTER_TEST(engine, "Docs", "open-quickly");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        connectEverything(ctx);
        const auto db = firstDbWithTables(ctx);
        if (!db) {
            ctx->LogError("no connected database with tables");
            return;
        }
        expandInSidebar(ctx, db);
        ctx->Yield(10);
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_P);
        ctx->Yield(2);
        ctx->KeyChars("inv");
        ctx->Yield(10);
        holdForScreenshot(ctx);
    };

    // saving a delete previews what foreign keys do to the rows referencing it
    t = IM_REGISTER_TEST(engine, "Docs", "delete-preview");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "shop.db";
        info.path = ":memory:";
        auto db = std::make_shared<FileDatabase>(info);
        if (!db->connect().first) {
            ctx->LogError("no sqlite");
            return;
        }
        for (const char* sql : {
                 "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, email TEXT)",
                 "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER "
                 "REFERENCES customers(id) ON DELETE CASCADE, total REAL)",
                 "CREATE TABLE order_items (id INTEGER PRIMARY KEY, order_id INTEGER "
                 "REFERENCES orders(id) ON DELETE CASCADE, sku TEXT)",
                 "CREATE TABLE reviews (id INTEGER PRIMARY KEY, customer_id INTEGER "
                 "REFERENCES customers(id) ON DELETE SET NULL, body TEXT)",
                 "CREATE TABLE invoices (id INTEGER PRIMARY KEY, customer_id INTEGER "
                 "REFERENCES customers(id), amount REAL)",
                 "INSERT INTO customers VALUES (1, 'Ada', 'ada@example.com'), "
                 "(2, 'Grace', 'grace@example.com'), (3, 'Linus', 'linus@example.com')",
                 "INSERT INTO orders VALUES (10, 1, 42.0), (11, 1, 18.5), (12, 1, 7.0), "
                 "(13, 2, 99.0)",
                 "INSERT INTO order_items VALUES (100, 10, 'A-1'), (101, 10, 'B-2'), "
                 "(102, 11, 'C-3'), (103, 12, 'A-1'), (104, 12, 'D-4'), (105, 13, 'E-5')",
                 "INSERT INTO reviews VALUES (1, 1, 'Great'), (2, 1, 'Fine')",
                 "INSERT INTO invoices VALUES (1, 1, 42.0)",
             })
            db->executeQuery(sql);
        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        const auto& tables = db->getTables();
        const auto customers = std::ranges::find(tables, std::string("customers"), &Table::name);
        if (customers == tables.end()) {
            ctx->LogError("no customers table");
            return;
        }
        app.addDatabase(db);
        const auto tab = std::dynamic_pointer_cast<TableViewerTab>(
            app.getTabManager()->createTableViewerTab(db.get(), *customers));
        ctx->Yield(40);
        tab->deleteRows({0});
        tab->saveChanges();
        for (int i = 0; i < 200 && !tab->deleteImpact(); ++i)
            ctx->Yield();
        ctx->Yield(10);
        holdForScreenshot(ctx);
    };

    // the inspector's Profile tab on a small orders table
    t = IM_REGISTER_TEST(engine, "Docs", "inspector-profile");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "shop.db";
        info.path = ":memory:";
        auto db = std::make_shared<FileDatabase>(info);
        if (!db->connect().first) {
            ctx->LogError("no sqlite");
            return;
        }
        db->executeQuery("CREATE TABLE orders (id INTEGER PRIMARY KEY, status TEXT NOT NULL, "
                         "total REAL, coupon TEXT, created_at TEXT)");
        db->executeQuery(
            "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 240) "
            "INSERT INTO orders (status, total, coupon, created_at) SELECT "
            "CASE WHEN i % 10 < 6 THEN 'paid' WHEN i % 10 < 9 THEN 'shipped' ELSE 'refunded' END, "
            "round(5 + (i * 37 % 200) + 0.99, 2), CASE WHEN i % 4 = 0 THEN 'SPRING10' END, "
            "date('2026-01-01', '+' || (i % 300) || ' days') FROM n");
        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        const auto& tables = db->getTables();
        const auto orders = std::ranges::find(tables, std::string("orders"), &Table::name);
        if (orders == tables.end()) {
            ctx->LogError("no orders table");
            return;
        }
        app.addDatabase(db);
        const auto tab = app.getTabManager()->createTableViewerTab(db.get(), *orders);
        ctx->Yield(20);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/##toggle_inspector");
        ctx->Yield(3);
        ctx->ItemClick("**/Profile");
        for (int i = 0; i < 200 && !ctx->ItemExists("**/Refresh"); ++i)
            ctx->Yield();
        holdForScreenshot(ctx);
    };

    // review indexes on a shop database: an unindexed foreign key and a prefix index
    t = IM_REGISTER_TEST(engine, "Docs", "review-indexes");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto db = makeShopDatabase("shop.db");
        if (!db)
            return ctx->LogError("no sqlite");
        loadTables(ctx, *db);
        auto* tabs = Application::getInstance().getTabManager();
        const size_t before = tabs->getTabCount();
        SchemaCompare::reviewIndexes(db.get());
        for (int i = 0; i < 300 && tabs->getTabCount() == before; ++i)
            ctx->Yield();
        holdForScreenshot(ctx);
    };

    // compare schema: dev has a column, a table and an index production lacks
    t = IM_REGISTER_TEST(engine, "Docs", "schema-compare");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto prod = makeShopDatabase("production.db");
        auto dev = makeShopDatabase("dev.db");
        if (!prod || !dev)
            return ctx->LogError("no sqlite");
        dev->executeQuery("ALTER TABLE customers ADD COLUMN phone TEXT");
        dev->executeQuery("CREATE TABLE refunds (id INTEGER PRIMARY KEY, order_id INTEGER NOT "
                          "NULL REFERENCES orders(id), amount REAL NOT NULL)");
        dev->executeQuery("CREATE INDEX orders_created_at ON orders (created_at)");
        loadTables(ctx, *prod);
        loadTables(ctx, *dev);
        auto* tabs = Application::getInstance().getTabManager();
        const size_t before = tabs->getTabCount();
        SchemaCompare::compare(prod.get(), dev.get());
        for (int i = 0; i < 300 && tabs->getTabCount() == before; ++i)
            ctx->Yield();
        holdForScreenshot(ctx);
    };

    // the chart tab under a query result
    t = IM_REGISTER_TEST(engine, "Docs", "results-chart");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "shop.db";
        info.path = ":memory:";
        auto db = std::make_shared<FileDatabase>(info);
        if (!db->connect().first) {
            ctx->LogError("no sqlite");
            return;
        }
        app.addDatabase(db);
        const auto editor = std::dynamic_pointer_cast<SQLEditorTab>(
            app.getTabManager()->createSQLEditorTab("", db.get()));
        editor->setQuery("with sales(month, revenue, cost) as (values\n"
                         "  ('Jan', 4200, 2600), ('Feb', 5100, 2900), ('Mar', 6300, 3100),\n"
                         "  ('Apr', 5800, 3300), ('May', 7200, 3600), ('Jun', 8100, 3900),\n"
                         "  ('Jul', 7600, 4000), ('Aug', 8800, 4200), ('Sep', 9400, 4300))\n"
                         "select * from sales;");
        ctx->Yield(10);
        ctx->SetRef(editor->getWindowName().c_str());
        ctx->ItemClick("**/Run");
        for (int i = 0; i < 300 && editor->getQueryResult().empty(); ++i)
            ctx->Yield();
        ctx->Yield(5);
        ctx->ItemClick("**/Chart");
        ctx->Yield(10);
        holdForScreenshot(ctx);
    };

    // the video opener (DEARSQL_RECORD): a million orders, paged, filtered and grouped
    t = IM_REGISTER_TEST(engine, "Docs", "showcase");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "warehouse";
        info.path = ":memory:";
        auto db = std::make_shared<FileDatabase>(info);
        if (!db->connect().first)
            return;
        for (const char* sql :
             {"CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, email TEXT, country "
              "TEXT, city TEXT, tier TEXT, signed_up TEXT, lifetime_value REAL)",
              "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100000) "
              "INSERT INTO customers SELECT i, "
              "json_extract('[\"Ada\",\"Grace\",\"Linus\",\"Margaret\",\"Alan\","
              "\"Barbara\",\"Dennis\",\"Frances\",\"Ken\",\"Radia\"]', '$[' || (i % 10) || ']') "
              "|| ' ' || json_extract('[\"Lovelace\",\"Hopper\",\"Torvalds\","
              "\"Hamilton\",\"Turing\",\"Liskov\",\"Ritchie\",\"Allen\",\"Thompson\","
              "\"Perlman\"]', '$[' || ((i / 10) % 10) || ']'), "
              "'user' || i || '@example.com', "
              "json_extract('[\"Japan\",\"Germany\",\"Brazil\",\"Vietnam\","
              "\"Canada\",\"France\",\"India\",\"Kenya\"]', '$[' || ((i * 7) % 8) || ']'), "
              "json_extract('[\"Tokyo\",\"Berlin\",\"Sao Paulo\",\"Hanoi\","
              "\"Toronto\",\"Paris\",\"Mumbai\",\"Nairobi\"]', '$[' || ((i * 7) % 8) || ']'), "
              "CASE i % 4 WHEN 0 THEN 'gold' WHEN 1 THEN 'silver' ELSE 'bronze' END, "
              "date('2019-01-01', '+' || ((i * 13) % 2500) || ' days'), "
              "round((i * 7919) % 500000 / 100.0, 2) FROM n",
              "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, product TEXT, "
              "category TEXT, status TEXT, quantity INTEGER, unit_price REAL, discount REAL, "
              "total REAL, currency TEXT, country TEXT, created_at TEXT, shipped_at TEXT)",
              "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < "
              "1000000) INSERT INTO orders SELECT i, 1 + (i * 7919) % 100000, "
              "'SKU-' || printf('%05d', (i * 31) % 5000), "
              "json_extract('[\"books\",\"games\",\"garden\",\"kitchen\","
              "\"music\",\"outdoor\",\"tools\",\"toys\"]', '$[' || ((i * 13) % 8) || ']'), "
              "CASE i % 10 WHEN 0 THEN 'refunded' WHEN 1 THEN 'pending' WHEN 2 THEN 'shipped' "
              "ELSE 'delivered' END, 1 + i % 9, round(5 + (i * 104729) % 49500 / 100.0, 2), "
              "CASE WHEN i % 7 = 0 THEN 0.1 ELSE 0 END, "
              "round((1 + i % 9) * (5 + (i * 104729) % 49500 / 100.0) * "
              "(CASE WHEN i % 7 = 0 THEN 0.9 ELSE 1 END), 2), "
              "CASE (i * 7) % 3 WHEN 0 THEN 'USD' WHEN 1 THEN 'EUR' ELSE 'JPY' END, "
              "json_extract('[\"Japan\",\"Germany\",\"Brazil\",\"Vietnam\","
              "\"Canada\",\"France\",\"India\",\"Kenya\"]', '$[' || ((i * 11) % 8) || ']'), "
              "datetime('2024-01-01', '+' || ((i * 37) % 31536000) || ' seconds'), "
              "datetime('2024-01-03', '+' || ((i * 37) % 31536000) || ' seconds') FROM n",
              "CREATE INDEX orders_customer ON orders (customer_id)"})
            db->executeQuery(sql);
        auto& app = Application::getInstance();
        app.addDatabase(db);
        app.setSidebarVisible(true);
        if (const char* record = std::getenv("DEARSQL_RECORD")) {
            const ImVec2 size = ImGui::GetIO().DisplaySize;
            if (!frame_capture::startRecording(record, 1920.0f / size.x, static_cast<int>(size.x),
                                               static_cast<int>(size.y)))
                ctx->LogError("could not start ffmpeg");
        }
        loadTables(ctx, *db);
        expandInSidebar(ctx, db);

        // watch it: real mouse moves and scrolls, not teleports
        const auto speed = ctx->EngineIO->ConfigRunSpeed;
        ctx->EngineIO->ConfigRunSpeed = ImGuiTestRunSpeed_Normal;
        auto* tabs = app.getTabManager();
        const auto& tables = db->getTables();
        const auto orders =
            std::ranges::find_if(tables, [](auto& t) { return t.name == "orders"; });
        if (orders == tables.end()) {
            ctx->LogError("no orders table");
            return;
        }
        auto tab = tabs->createTableViewerTab(db.get(), *orders);
        ctx->Yield(30);
        ctx->SetRef(tab->getWindowName().c_str());
        for (int i = 0; i < 600 && !ctx->ItemExists("**/>>"); ++i)
            ctx->Yield();
        ctx->MouseMoveToPos(ctx->GetWindowByRef("")->Rect().GetCenter());
        for (int i = 0; i < 40; ++i) {
            ctx->MouseWheelY(-4.0f);
            ctx->Yield(2);
        }
        for (int i = 0; i < 6; ++i) {
            ctx->ItemClick("**/>");
            ctx->Yield(6);
        }
        ctx->ItemClick("**/>>");
        ctx->Yield(20);
        ctx->ItemClick("**/##filter");
        ctx->KeyCharsAppend("total > 4000 AND country = 'Japan' AND status = 'delivered'");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(40);
        for (int i = 0; i < 20; ++i) {
            ctx->MouseWheelY(-4.0f);
            ctx->Yield(2);
        }

        const auto customers =
            std::ranges::find_if(tables, [](auto& t) { return t.name == "customers"; });
        auto ctab = tabs->createTableViewerTab(db.get(), *customers);
        ctx->Yield(30);
        ctx->SetRef(ctab->getWindowName().c_str());
        ctx->MouseMoveToPos(ctx->GetWindowByRef("")->Rect().GetCenter());
        for (int i = 0; i < 20; ++i) {
            ctx->MouseWheelY(-4.0f);
            ctx->Yield(2);
        }

        // a group-by over the million rows, then its chart
        const auto etab = tabs->createSQLEditorTab("", db.get());
        std::dynamic_pointer_cast<SQLEditorTab>(etab)->setQuery(
            "-- revenue per category, over 1,000,000 orders\n"
            "SELECT category, count(*) AS orders, round(sum(total)) AS revenue,\n"
            "       round(avg(total), 2) AS avg_order\n"
            "FROM orders\nWHERE status <> 'refunded'\nGROUP BY category\nORDER BY revenue DESC;");
        ctx->Yield(10);
        ctx->SetRef(etab->getWindowName().c_str());
        ctx->ItemClick("**/Run");
        for (int i = 0; i < 600 && !ctx->ItemExists("**/Chart"); ++i)
            ctx->Yield();
        ctx->Yield(60); // the grid, before the chart
        ctx->ItemClick("**/Chart");
        ctx->MouseMoveToPos(ctx->GetWindowByRef("")->Rect().GetCenter());
        ctx->Yield(150);
        ctx->EngineIO->ConfigRunSpeed = speed;
    };
}
