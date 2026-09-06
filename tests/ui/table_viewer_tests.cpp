#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/sqlite.hpp"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/tab/table_viewer_tab.hpp"
#include "ui/tab_manager.hpp"
#include <algorithm>
#include <memory>

namespace {
    // in-memory sqlite with a foreign key, so the test needs no fixture on disk
    std::shared_ptr<SQLiteDatabase> makeFkDatabase() {
        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = "FkTestDB";
        info.path = ":memory:";

        auto db = std::make_shared<SQLiteDatabase>(info);
        if (auto [ok, err] = db->connect(); !ok) {
            return nullptr;
        }
        db->executeQuery("CREATE TABLE parent (id INTEGER PRIMARY KEY, label TEXT)");
        db->executeQuery("CREATE TABLE child (id INTEGER PRIMARY KEY, "
                         "parent_id INTEGER REFERENCES parent(id))");
        db->executeQuery("INSERT INTO parent (id,label) VALUES (7,'seven')");
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
        ctx->ItemClick("**/##fk_go");
        ctx->Yield(40);
        IM_CHECK(tabs->getTabCount() == before + 1);

        tabs->closeTabsForDatabase(db.get());
        app.removeDatabase(db);
        ctx->Yield();
    };
}
