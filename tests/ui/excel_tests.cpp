#include "application.hpp"
#include "app_state.hpp"
#include "database/file_database.hpp"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/tab_manager.hpp"
#include "utils/xlsx.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>

void RegisterExcelTests(ImGuiTestEngine* engine) {
    // a workbook opens like a database file: a connection, its sheets as tables, rows in
    // the viewer, and edits refused
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Excel", "Workbook opens as tables");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const auto path = (std::filesystem::temp_directory_path() /
                           std::format("dearsql_ui_{}.xlsx",
                                       std::chrono::steady_clock::now().time_since_epoch().count()))
                              .string();
        {
            xlsx::Writer w(path);
            w.beginSheet("people", {"id", "name"}, {"integer", "text"});
            w.addRow({"1", "ada_lovelace"});
            w.addRow({"2", "grace_hopper"});
            std::string error;
            IM_CHECK_SILENT(w.save(error));
        }
        auto& app = Application::getInstance();
        app.openFile(path);
        std::shared_ptr<DatabaseInterface> db;
        for (const auto& d : app.getDatabases())
            if (auto* f = dynamic_cast<FileDatabase*>(d.get()); f && std::filesystem::exists(f->getPath()) &&
                                                           std::filesystem::equivalent(f->getPath(), path))
                db = d;
        IM_CHECK_SILENT(db != nullptr);
        IM_CHECK(db->getConnectionInfo().readOnly);
        // connected off the ui thread, like the sidebar does
        for (int i = 0; i < 300 && !db->isConnected(); ++i) {
            db->checkConnectionStatusAsync();
            ctx->Yield();
        }
        IM_CHECK_SILENT(db->isConnected());
        auto* file = dynamic_cast<FileDatabase*>(db.get());
        file->startTablesLoadAsync();
        for (int i = 0; i < 300 && !file->isTablesLoaded(); ++i) {
            file->checkLoadingStatus();
            ctx->Yield();
        }
        const auto& tables = file->getTables();
        const auto people =
            std::ranges::find(tables, std::string("people"), &Table::name);
        IM_CHECK_SILENT(people != tables.end());

        auto* tabs = app.getTabManager();
        const auto tab = tabs->createTableViewerTab(file, *people);
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        for (int i = 0; i < 300 && !ctx->ItemExists("**/grace_hopper"); ++i)
            ctx->Yield();
        IM_CHECK(ctx->ItemExists("**/ada_lovelace"));

        const auto write = file->executeQuery("DELETE FROM people");
        IM_CHECK(!write.success());

        tabs->closeTabsForDatabase(file);
        app.getAppState()->deleteConnection(db->getConnectionId());
        app.removeDatabase(db);
        ctx->Yield();
        std::filesystem::remove(path);
    };
}
