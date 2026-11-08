#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/schema_compare.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab/table_viewer_tab.hpp"
#include "ui/tab_manager.hpp"
#include <algorithm>
#include <chrono>
#include <memory>

namespace {
    // in-memory sqlite with a foreign key, so the test needs no fixture on disk
    std::shared_ptr<FileDatabase> makeFkDatabase() {
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "FkTestDB";
        info.path = ":memory:";

        auto db = std::make_shared<FileDatabase>(info);
        if (auto [ok, err] = db->connect(); !ok) {
            return nullptr;
        }
        db->executeQuery("CREATE TABLE parent (id INTEGER PRIMARY KEY, label TEXT)");
        db->executeQuery("CREATE TABLE child (id INTEGER PRIMARY KEY, "
                         "parent_id INTEGER REFERENCES parent(id))");
        // three parent rows: a filtered load returns one, an unfiltered load three
        db->executeQuery("INSERT INTO parent (id,label) VALUES (6,'six'),(7,'seven'),(8,'eight')");
        db->executeQuery("INSERT INTO child (id,parent_id) VALUES (1,7)");
        return db;
    }

    const Table* findTable(const std::vector<Table>& tables, const std::string& name) {
        const auto it =
            std::ranges::find_if(tables, [&](const Table& t) { return t.name == name; });
        return it == tables.end() ? nullptr : &*it;
    }
} // namespace

void RegisterTableViewerTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    // the inspector's Profile tab reads the table on a worker and lists its columns
    t = IM_REGISTER_TEST(engine, "TableViewer", "Inspector profiles columns");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        const Table* parent = findTable(db->getTables(), "parent");
        IM_CHECK_SILENT(parent != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab = tabs->createTableViewerTab(db.get(), *parent);
        ctx->Yield(20);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/##toggle_inspector");
        ctx->Yield(3);
        ctx->ItemClick("**/Profile");
        bool shown = false;
        for (int i = 0; i < 200 && !shown; ++i) {
            ctx->Yield();
            shown = ctx->ItemExists("**/Refresh"); // the profile is in, the button back
        }
        IM_CHECK(shown);
        tabs->closeAllTabs();
        app.removeDatabase(db);
    };

    // profile columns runs its query in a new editor; compare opens the diff unrun
    t = IM_REGISTER_TEST(engine, "Schema", "Profile and compare open scripts");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto prod = makeFkDatabase();
        auto dev = makeFkDatabase();
        IM_CHECK_SILENT(prod && dev);
        dev->executeQuery("ALTER TABLE parent ADD COLUMN note TEXT");
        for (auto& db : {prod, dev}) {
            db->startTablesLoadAsync(true);
            for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
                db->checkLoadingStatus();
                ctx->Yield();
            }
            app.addDatabase(db);
        }
        auto* tabs = app.getTabManager();

        SchemaCompare::openProfile(prod.get(), *findTable(prod->getTables(), "parent"));
        auto profile = std::dynamic_pointer_cast<SQLEditorTab>(tabs->getTabs().back());
        IM_CHECK_SILENT(profile != nullptr);
        for (int i = 0; i < 200 && profile->getQueryResult().statements.empty(); ++i)
            ctx->Yield();
        IM_CHECK_SILENT(!profile->getQueryResult().statements.empty());
        const auto& rows = profile->getQueryResult().statements[0].tableData;
        IM_CHECK_EQ(rows.size(), 2u);             // id, label
        IM_CHECK_STR_EQ(rows[0][3].c_str(), "3"); // distinct ids

        const size_t before = tabs->getTabCount();
        SchemaCompare::compare(prod.get(), dev.get());
        for (int i = 0; i < 300 && tabs->getTabCount() == before; ++i)
            ctx->Yield();
        auto diff = std::dynamic_pointer_cast<SQLEditorTab>(tabs->getTabs().back());
        IM_CHECK_SILENT(diff != nullptr && diff != profile);
        IM_CHECK(diff->getQuery().find("ALTER TABLE \"parent\" ADD COLUMN \"note\" TEXT;") !=
                 std::string::npos);
        IM_CHECK(diff->getQueryResult().statements.empty()); // nothing ran

        // child.parent_id references parent with no index on it
        const size_t beforeReview = tabs->getTabCount();
        SchemaCompare::reviewIndexes(prod.get());
        for (int i = 0; i < 300 && tabs->getTabCount() == beforeReview; ++i)
            ctx->Yield();
        auto review = std::dynamic_pointer_cast<SQLEditorTab>(tabs->getTabs().back());
        IM_CHECK_SILENT(review != nullptr && review != diff);
        IM_CHECK(review->getQuery().find("[unindexed-fk] child") != std::string::npos);
        IM_CHECK(review->getQuery().find("CREATE INDEX") != std::string::npos);
        IM_CHECK(review->getQueryResult().statements.empty());

        tabs->closeAllTabs();
        app.removeDatabase(prod);
        app.removeDatabase(dev);
    };

    // regression: following a foreign key opens a tab from inside another tab's
    // render. renderTabs() holds an iterator into `tabs` while rendering, so
    // pushing there invalidated it and crashed. the open has to be deferred.
    t = IM_REGISTER_TEST(engine, "TableViewer", "Follow Foreign Key");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);

        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }

        const Table* child = findTable(db->getTables(), "child");
        IM_CHECK_SILENT(child != nullptr);

        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab = tabs->createTableViewerTab(db.get(), *child);
        ctx->Yield(40); // let the first page load so the cell exists

        const size_t before = tabs->getTabCount();

        // the cell is a Selectable whose id comes from its text
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/7", ImGuiMouseButton_Right);
        ctx->Yield(3);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("**/" ICON_FA_ARROW_RIGHT " Go to parent");

        // the crash landed here: the deferred open runs after the render loop
        ctx->Yield(40);
        IM_CHECK(tabs->getTabCount() == before + 1);

        // the target tab must actually be filtered: its own first load is already
        // in flight when setFilter runs, and an unfiltered result would show all
        // three parent rows while claiming the fk filter is active
        const auto opened = std::dynamic_pointer_cast<TableViewerTab>(tabs->getTabs().back());
        IM_CHECK_SILENT(opened != nullptr);
        IM_CHECK(!opened->getCurrentFilter().empty());
        IM_CHECK(opened->getTotalRows() == 1);

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // the jump button that appears on a hovered or selected foreign-key cell
    t = IM_REGISTER_TEST(engine, "TableViewer", "Foreign Key Jump Button");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);

        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        const Table* child = findTable(db->getTables(), "child");
        IM_CHECK_SILENT(child != nullptr);

        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab = tabs->createTableViewerTab(db.get(), *child);
        ctx->Yield(40);

        const size_t before = tabs->getTabCount();

        // hovering the fk cell reveals the button; clicking it navigates
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->MouseMove("**/7");
        ctx->Yield(3);

        // click the far right edge, not the centre: placing the button against
        // the cell's *background* rect put half of it outside the content clip
        // rect, where imgui will not hit-test it
        const ImGuiTestItemInfo info = ctx->ItemInfo("**/##fk_go");
        IM_CHECK_SILENT(info.ID != 0);
        ctx->MouseMoveToPos(ImVec2(info.RectFull.Max.x - 1.0f, info.RectFull.GetCenter().y));
        ctx->MouseClick(ImGuiMouseButton_Left);

        ctx->Yield(40);
        IM_CHECK(tabs->getTabCount() == before + 1);

        // the target tab must actually be filtered: its own first load is already
        // in flight when setFilter runs, and an unfiltered result would show all
        // three parent rows while claiming the fk filter is active
        const auto opened = std::dynamic_pointer_cast<TableViewerTab>(tabs->getTabs().back());
        IM_CHECK_SILENT(opened != nullptr);
        IM_CHECK(!opened->getCurrentFilter().empty());
        IM_CHECK(opened->getTotalRows() == 1);

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // regression: dropping a database (or a schema leaving a relist) freed the node
    // while tabs on it, or on a schema under it, kept rendering it
    t = IM_REGISTER_TEST(engine, "TabManager", "Close Tabs Under Retired Node");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* tabs = Application::getInstance().getTabManager();
        // nodes off any connection: nothing here opens a handle
        auto dbNode = std::make_unique<PostgresDatabaseNode>();
        dbNode->name = "retired_db";
        auto schema = std::make_unique<PostgresSchemaNode>();
        schema->name = "s1";
        schema->parentDbNode = dbNode.get();
        auto* schemaPtr = schema.get();
        dbNode->schemas.push_back(std::move(schema));

        const size_t before = tabs->getTabCount();
        tabs->createSQLEditorTab("", dbNode.get());
        tabs->createSQLEditorTab("", schemaPtr);
        IM_CHECK(tabs->getTabCount() == before + 2);

        // the hook path the app installs; no frame runs in between
        tabs->retireNode(std::move(dbNode));
        IM_CHECK(tabs->getTabCount() == before);
        ctx->Yield();
    };

    // explain takes the statement under the cursor (here: after its closing ';')
    // and shows the parsed plan instead of a result grid
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Explain shows the plan");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        tab->setQuery("select 1;\nselect * from child c join parent p on p.id = c.parent_id;");
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/Explain");
        for (int i = 0; i < 300 && !tab->getPlan(); ++i) {
            ctx->Yield();
        }
        IM_CHECK(tab->getPlan().has_value());
        IM_CHECK(tab->getPlan()->root.children.size() == 2u); // scan child, search parent
        IM_CHECK(tab->getPlan()->root.children[0].operation.starts_with("SCAN"));

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // the gutter play icon runs just its statement, no selection needed
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Gutter play runs one statement");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        tab->setQuery("select 1 as first;\n-- a comment; with a semicolon\nselect label as second\n"
                      "from parent\nwhere id = 7;");
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/##run_1");
        for (int i = 0; i < 300 && tab->getQueryResult().empty(); ++i) {
            ctx->Yield();
        }
        const auto& result = tab->getQueryResult();
        IM_CHECK(result.size() == 1u); // only the second statement ran
        IM_CHECK(result[0].columnNames == std::vector<std::string>{"second"});
        IM_CHECK(result[0].tableData.size() == 1u && result[0].tableData[0][0] == "seven");

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // a syntax error is underlined at the token the parser stopped on
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Syntax error is underlined");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        const std::string sql = "select 1;\nselect *\nfrm parent;";
        tab->setQuery(sql);
        for (int i = 0; i < 120 && tab->getEditor().GetErrorOffset() < 0; ++i) {
            ctx->Yield();
        }
        IM_CHECK_EQ(tab->getEditor().GetErrorOffset(), static_cast<int>(sql.find("frm")));

        tab->setQuery("select 1;\nselect * from parent;"); // fixed: the marker goes
        for (int i = 0; i < 120 && tab->getEditor().GetErrorOffset() >= 0; ++i) {
            ctx->Yield();
        }
        IM_CHECK_EQ(tab->getEditor().GetErrorOffset(), -1);

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // issue 53: a postgres schema with ~1000 tables and views must load and draw quickly.
    // needs DEARSQL_TEST_PG_* and DEARSQL_TEST_PG_BIG_DB (a database with that many objects)
    t = IM_REGISTER_TEST(engine, "Sidebar", "Large postgres schema");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* host = std::getenv("DEARSQL_TEST_PG_HOST");
        const char* bigDb = std::getenv("DEARSQL_TEST_PG_BIG_DB");
        if (!host || !bigDb) {
            return; // skipped
        }
        auto env = [](const char* name, const char* fallback) {
            const char* v = std::getenv(name);
            return std::string(v ? v : fallback);
        };
        DatabaseConnectionInfo info;
        info.name = "big";
        info.type = DatabaseType::POSTGRESQL;
        info.host = host;
        info.port = std::stoi(env("DEARSQL_TEST_PG_PORT", "5432"));
        info.database = bigDb;
        info.username = env("DEARSQL_TEST_PG_USER", "postgres");
        info.password = env("DEARSQL_TEST_PG_PASSWORD", "");
        auto db = std::make_shared<PostgresDatabase>(info);
        IM_CHECK_SILENT(db->connect().first);
        auto& app = Application::getInstance();
        app.addDatabase(db);
        using Clock = std::chrono::steady_clock;
        auto ms = [](Clock::time_point a) {
            return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        };

        ctx->SetRef("Databases");
        ctx->ItemOpen(std::format("**/###db_{:p}", static_cast<const void*>(db.get())).c_str());
        auto* node = dynamic_cast<PostgresDatabaseNode*>(db->getDatabaseData(bigDb));
        IM_CHECK_SILENT(node != nullptr);
        ctx->ItemOpen(
            std::format("**/###db_{}_{:p}", node->name, static_cast<void*>(node)).c_str());
        for (int i = 0; i < 600 && !node->schemasLoaded; ++i) {
            ctx->Yield();
        }
        PostgresSchemaNode* schema = nullptr;
        for (auto& s : node->schemas) {
            if (s->name == "public") {
                schema = s.get();
            }
        }
        IM_CHECK_SILENT(schema != nullptr);
        ctx->ItemOpen(
            std::format("**/###schema_{}_{:p}", schema->name, static_cast<void*>(schema)).c_str());
        ctx->Yield(3);
        auto frameMs = [&] {
            const auto start = Clock::now();
            ctx->Yield(60);
            return ms(start) / 60.0;
        };
        const double closedFrame = frameMs();

        const auto loadStart = Clock::now();
        ctx->ItemOpen(
            std::format("**/###tables_{}_{:p}", schema->name, static_cast<void*>(&schema->tables))
                .c_str());
        ctx->ItemOpen(
            std::format("**/###views_{}_{:p}", schema->name, static_cast<void*>(&schema->views))
                .c_str());
        for (int i = 0; i < 3000 && (!schema->tablesLoaded || !schema->viewsLoaded); ++i) {
            ctx->Yield();
        }
        const double loadMs = ms(loadStart);
        const double openFrame = frameMs();
        ctx->LogInfo("tables %zu views %zu: load %.0f ms, frame %.2f ms closed, %.2f ms open",
                     schema->tables.size(), schema->views.size(), loadMs, closedFrame, openFrame);
        fprintf(stderr,
                "issue53: tables %zu views %zu: load %.0f ms, frame %.2f ms closed, %.2f ms open\n",
                schema->tables.size(), schema->views.size(), loadMs, closedFrame, openFrame);
        IM_CHECK(schema->tables.size() >= 789u);

        // skipped rows keep their height: scrolled to the bottom, the last view is on screen
        const auto tablesItem = ctx->ItemInfo(
            std::format("**/###tables_{}_{:p}", schema->name, static_cast<void*>(&schema->tables))
                .c_str());
        IM_CHECK_SILENT(tablesItem.Window != nullptr);
        ctx->ScrollToBottom(tablesItem.Window->ID);
        ctx->Yield(3);
        const Table& last = schema->views.back();
        const auto lastItem = ctx->ItemInfo(
            std::format("**/###view_{}_{:p}", last.name, static_cast<const void*>(&last)).c_str(),
            ImGuiTestOpFlags_NoError);
        IM_CHECK(lastItem.ID != 0);

        app.getTabManager()->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // chart tab: picks columns, and survives a re-run with fewer columns
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Results chart");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        const auto run = [&](const char* sql, const std::string& firstColumn) {
            tab->setQuery(sql);
            ctx->Yield(2);
            ctx->ItemClick("**/Run");
            for (int i = 0; i < 300; ++i) {
                const auto& r = tab->getQueryResult();
                if (r.size() == 1 && !r[0].columnNames.empty() &&
                    r[0].columnNames[0] == firstColumn)
                    break;
                ctx->Yield();
            }
            ctx->Yield(2);
        };
        ctx->SetRef(tab->getWindowName().c_str());
        run("select 'a' as k, 1 as x, 2 as y union all select 'b', 3, 4", "k");
        ctx->ItemClick("**/Chart");
        ctx->Yield(2);
        IM_CHECK(tab->chart().visible());
        IM_CHECK_EQ(tab->chart().settings().x, 0);
        IM_CHECK(tab->chart().settings().ys == (std::vector<int>{1, 2}));
        IM_CHECK_EQ(tab->chart().data().series.size(), 2u);
        auto pie = tab->chart().settings();
        pie.kind = chart::Kind::Pie;
        pie.ys = {2};
        tab->chart().setSettings(pie);
        ctx->Yield(2);
        IM_CHECK_EQ(tab->chart().data().series.size(), 1u);

        run("select 5 as only_col", "only_col"); // the old y index is gone
        IM_CHECK(tab->chart().visible());
        IM_CHECK(tab->chart().settings().ys == (std::vector<int>{0}));
        IM_CHECK_EQ(tab->chart().data().labels.size(), 1u);

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // data / json tabs under the results and under a table
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Results JSON view");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        tab->setQuery("select 1 as a, null as b");
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/Run");
        for (int i = 0; i < 300 && tab->getQueryResult().empty(); ++i) {
            ctx->Yield();
        }
        ctx->Yield(2);
        IM_CHECK(!tab->jsonView().visible());
        ctx->ItemClick("**/JSON");
        ctx->Yield(2);
        IM_CHECK(tab->jsonView().visible());
        IM_CHECK(tab->jsonView().text().find("\"b\": null") != std::string::npos);
        ctx->ItemClick("**/Data");

        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        const Table* child = findTable(db->getTables(), "child");
        IM_CHECK_SILENT(child != nullptr);
        const auto viewer = tabs->createTableViewerTab(db.get(), *child);
        ctx->Yield(40);
        ctx->SetRef(viewer->getWindowName().c_str());
        ctx->ItemClick("**/JSON");
        ctx->Yield(2);
        const auto* viewerTab = dynamic_cast<TableViewerTab*>(viewer.get());
        IM_CHECK_SILENT(viewerTab != nullptr);
        IM_CHECK(viewerTab->jsonView().visible());
        IM_CHECK(viewerTab->jsonView().text().starts_with("[\n  {"));
        ctx->ItemClick("**/Chart");
        ctx->Yield(2);
        IM_CHECK(viewerTab->chart().visible());
        IM_CHECK(!viewerTab->chart().data().labels.empty());

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // :name placeholders ask for values, then run with them as literals
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Query parameters prompt");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        tab->setQuery("select :a as x, ':not_me' as y, :b as z, :a + 1 as w");
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->ItemClick("**/Run");
        ctx->Yield(2);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemInputValue("##param_a", "7");
        IM_CHECK(ctx->ItemInfo("##param_not_me", ImGuiTestOpFlags_NoError).ID == 0);
        IM_CHECK(tab->getQueryResult().empty());  // enter in :a moved on, ran nothing
        ctx->ItemInputValue("##param_b", "it's"); // enter in the last field runs
        for (int i = 0; i < 300 && tab->getQueryResult().empty(); ++i) {
            ctx->Yield();
        }
        const auto& result = tab->getQueryResult();
        IM_CHECK_SILENT(result.size() == 1u && !result[0].tableData.empty());
        IM_CHECK(result[0].tableData[0] == (std::vector<std::string>{"7", ":not_me", "it's", "8"}));

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // cmd/ctrl+p finds a loaded table by a sloppy name and opens it
    t = IM_REGISTER_TEST(engine, "OpenQuickly", "Opens a table by name");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const size_t before = tabs->getTabCount();

        // esc closes it, even with the text field focused
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_P);
        ctx->Yield(2);
        IM_CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));

        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_P);
        ctx->Yield(2);
        ctx->KeyChars("chlid"); // a typo still finds child
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);

        IM_CHECK_SILENT(tabs->getTabCount() == before + 1);
        const auto viewer = std::dynamic_pointer_cast<TableViewerTab>(tabs->getTabs().back());
        IM_CHECK(viewer != nullptr);
        IM_CHECK(viewer && viewer->getWindowName().find("child") != std::string::npos);

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // the performance settings persist (the frame cap lives in Application::run, not the test loop)
    t = IM_REGISTER_TEST(engine, "Settings", "Frame rate settings");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        const bool fps = app.showFps(), vsync = app.vsync();
        const int limit = app.maxFps();
        app.setShowFps(!fps);
        app.setVSync(false);
        app.setMaxFps(30);
        IM_CHECK(app.getAppState()->getSetting("show_fps", "") == (fps ? "0" : "1"));
        IM_CHECK(app.getAppState()->getSetting("vsync", "") == "0");
        IM_CHECK(app.getAppState()->getSetting("max_fps", "") == "30");
        app.setShowFps(fps);
        app.setVSync(vsync);
        app.setMaxFps(limit);
    };

    // edits recolor only what changed; the result must match highlighting from scratch
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Incremental highlight matches full");
    t->TestFunc = [](ImGuiTestContext*) {
        dearsql::TextEditor editor;
        std::string sql;
        for (int i = 0; i < 50; ++i)
            sql += "select a, 'x', 42 from t where b = 1; -- note\n";
        editor.SetText(sql);
        (void)editor.GetColors();
        const std::vector<std::pair<size_t, std::string>> inserts = {
            {120, "/*"},   // opens a comment to the end
            {900, "*/"},   // and closes it far below
            {300, "'"},    // an unclosed string
            {301, "abc'"}, // closed again
            {0, "with q as (select 1) "},
            {100000, "update t set c = 'y';\n"},
        };
        for (const auto& [at, text] : inserts) {
            auto current = editor.GetText();
            current.insert(std::min(at, current.size()), text);
            editor.SetText(current);
            dearsql::TextEditor fresh;
            fresh.SetText(current);
            IM_CHECK(editor.GetColors() == fresh.GetColors());
        }
        auto current = editor.GetText();
        current.erase(100, 400); // a big delete
        editor.SetText(current);
        dearsql::TextEditor fresh;
        fresh.SetText(current);
        IM_CHECK(editor.GetColors() == fresh.GetColors());
    };

    // opt+backspace (ctrl elsewhere) deletes a word, cmd+backspace (ctrl+shift) to the line start
    t = IM_REGISTER_TEST(engine, "SQLEditor", "Backspace deletes a word or the line");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        auto db = makeFkDatabase();
        IM_CHECK_SILENT(db != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab =
            std::dynamic_pointer_cast<SQLEditorTab>(tabs->createSQLEditorTab("", db.get()));
        IM_CHECK_SILENT(tab != nullptr);
        tab->setQuery("select 1;\nselect foo_bar,  baz.qux  ");
        ctx->Yield(3);
        const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
        const ImGuiKeyChord word = (mac ? ImGuiMod_Alt : ImGuiMod_Ctrl) | ImGuiKey_Backspace;
        const ImGuiKeyChord line =
            (mac ? ImGuiMod_Super : ImGuiMod_Ctrl | ImGuiMod_Shift) | ImGuiKey_Backspace;
        ctx->KeyPress(word); // trailing spaces and "qux"
        IM_CHECK_STR_EQ(tab->getEditor().GetText().c_str(), "select 1;\nselect foo_bar,  baz.");
        ctx->KeyPress(word); // "."
        IM_CHECK_STR_EQ(tab->getEditor().GetText().c_str(), "select 1;\nselect foo_bar,  baz");
        ctx->KeyPress(line);
        IM_CHECK_STR_EQ(tab->getEditor().GetText().c_str(), "select 1;\n");
        ctx->KeyPress(line); // at the line start: joins with the line above
        IM_CHECK_STR_EQ(tab->getEditor().GetText().c_str(), "select 1;");

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };

    // saving deletes previews what foreign keys do to the referencing rows, and the counts
    // are what the delete then does
    t = IM_REGISTER_TEST(engine, "TableViewer", "Delete preview counts referencing rows");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "ImpactDB";
        info.path = ":memory:";
        auto db = std::make_shared<FileDatabase>(info);
        IM_CHECK_SILENT(db->connect().first);
        for (const char* sql : {
                 "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT)",
                 "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER "
                 "REFERENCES customers(id) ON DELETE CASCADE)",
                 "CREATE TABLE notes (id INTEGER PRIMARY KEY, customer_id INTEGER "
                 "REFERENCES customers(id) ON DELETE SET NULL)",
                 "CREATE TABLE invoices (id INTEGER PRIMARY KEY, customer_id INTEGER "
                 "REFERENCES customers(id))",
                 "INSERT INTO customers VALUES (1, 'a'), (2, 'b')",
                 "INSERT INTO orders VALUES (10, 1), (11, 1), (12, 2)",
                 "INSERT INTO notes VALUES (1, 1)",
                 "INSERT INTO invoices VALUES (1, 2)",
             })
            db->executeQuery(sql);
        db->startTablesLoadAsync(true);
        for (int i = 0; i < 200 && !db->isTablesLoaded(); ++i) {
            db->checkLoadingStatus();
            ctx->Yield();
        }
        const Table* customers = findTable(db->getTables(), "customers");
        IM_CHECK_SILENT(customers != nullptr);
        app.addDatabase(db);
        auto* tabs = app.getTabManager();
        const auto tab = std::dynamic_pointer_cast<TableViewerTab>(
            tabs->createTableViewerTab(db.get(), *customers));
        IM_CHECK_SILENT(tab != nullptr);
        ctx->Yield(40);

        auto preview = [&](int row) -> const TableViewerTab::DeleteImpact* {
            tab->deleteRows({row});
            tab->saveChanges();
            for (int i = 0; i < 200 && !tab->deleteImpact(); ++i)
                ctx->Yield();
            return tab->deleteImpact() ? &*tab->deleteImpact() : nullptr;
        };
        auto countFor = [](const TableViewerTab::DeleteImpact& impact, const std::string& table) {
            for (const auto& r : impact.rows)
                if (r.effect.table == table)
                    return r.count;
            return -2LL;
        };

        // customer 1: two orders cascade, one note is set to null, nothing blocks
        const auto* impact = preview(0);
        IM_CHECK_SILENT(impact != nullptr);
        IM_CHECK(impact->enforced);
        IM_CHECK(countFor(*impact, "orders") == 2);
        IM_CHECK(countFor(*impact, "notes") == 1);
        IM_CHECK(countFor(*impact, "invoices") == 0);
        ctx->ItemClick("//Confirm Save Changes/**/" ICON_FA_PLAY " Execute");
        for (int i = 0; i < 100 && tab->deleteImpact(); ++i)
            ctx->Yield(); // the dialog closes once the delete ran
        const auto left = [&](const char* sql) {
            const auto r = db->executeQuery(sql);
            return r.success() && !r.empty() ? r[0].tableData[0][0] : std::string("?");
        };
        IM_CHECK_STR_EQ(left("SELECT COUNT(*) FROM orders").c_str(), "1");
        IM_CHECK_STR_EQ(left("SELECT COUNT(*) FROM notes WHERE customer_id IS NULL").c_str(), "1");
        ctx->Yield(40); // the reload after saving

        // customer 2: its invoice blocks the delete
        impact = preview(0);
        IM_CHECK_SILENT(impact != nullptr);
        IM_CHECK(countFor(*impact, "invoices") == 1);
        for (const auto& r : impact->rows)
            if (r.effect.table == "invoices")
                IM_CHECK_STR_EQ(r.effect.action.c_str(), "NO ACTION");

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };
}
