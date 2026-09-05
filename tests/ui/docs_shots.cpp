#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/ai_settings_dialog.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/tab/sql_editor_tab.hpp"
#include "ui/tab_manager.hpp"
#include <chrono>
#include <format>

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
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < until) {
            ctx->Yield();
        }
    }

    void openAssistant(ImGuiTestContext* ctx) {
        Application::getInstance().setSidebarVisible(true);
        ctx->Yield(2);
        ctx->SetRef("Databases");
        ctx->ItemClick("**/##sidebar_tab_1");
        ctx->Yield(3);
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
        ctx->ItemClick("**/##sidebar_tab_0");
        ctx->Yield(3);
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
        Application::getInstance().getTabManager()->createTableViewerTab(node,
                                                                         node->getTables().front());
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

    // the AI settings dialog, for the database-tools page
    t = IM_REGISTER_TEST(engine, "Docs", "ai-settings");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistant(ctx);
        AISettingsDialog::instance().show();
        ctx->Yield(5);
        holdForScreenshot(ctx);
    };
}
