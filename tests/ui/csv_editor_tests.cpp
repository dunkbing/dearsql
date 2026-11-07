#include "application.hpp"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/tab/csv_editor_tab.hpp"
#include "ui/tab_manager.hpp"
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>

namespace {
    std::filesystem::path writeTempCsv(const std::string& bytes) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path =
            std::filesystem::temp_directory_path() / std::format("dearsql_csv_ui_{}.csv", stamp);
        std::ofstream(path, std::ios::binary) << bytes;
        return path;
    }

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // opens the editor and waits for the file to load
    std::shared_ptr<Tab> openEditor(ImGuiTestContext* ctx, const std::filesystem::path& path,
                                    const char* firstCell) {
        auto tab = Application::getInstance().getTabManager()->createCsvEditorTab(path.string());
        ctx->Yield(3);
        ctx->SetRef(tab->getWindowName().c_str());
        for (int i = 0; i < 300 && !ctx->ItemExists(std::format("**/{}", firstCell).c_str()); ++i)
            ctx->Yield();
        return tab;
    }
} // namespace

void RegisterCsvEditorTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    // an edit saves in the file's own format: semicolons and CRLF survive
    t = IM_REGISTER_TEST(engine, "CsvEditor", "Edit Save Undo");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const auto path = writeTempCsv("id;name\r\n1;ada\r\n2;grace\r\n");
        auto tab = openEditor(ctx, path, "ada");
        IM_CHECK(ctx->ItemExists("**/ada"));

        ctx->ItemDoubleClick("**/ada");
        ctx->Yield(3);
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_A);
        ctx->KeyChars("linus");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK(tab->hasUnsavedChanges());

        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_S);
        ctx->Yield(3);
        IM_CHECK(!tab->hasUnsavedChanges());
        IM_CHECK_EQ(readFile(path), std::string("id;name\r\n1;linus\r\n2;grace\r\n"));

        // undo brings the old value back and marks the tab unsaved again
        ctx->ItemClick("**/grace");
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_Z);
        ctx->Yield(3);
        IM_CHECK(ctx->ItemExists("**/ada"));
        IM_CHECK(tab->hasUnsavedChanges());

        // save, so closing the tab does not ask
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_S);
        ctx->Yield(3);
        tab->setOpen(false);
        ctx->Yield(3);
        std::filesystem::remove(path);
    };

    // the WHERE filter runs on DuckDB over the saved file and keeps rows editable
    t = IM_REGISTER_TEST(engine, "CsvEditor", "DuckDB Filter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const auto path = writeTempCsv("id,name\n1,ada\n2,grace\n3,linus\n");
        auto tab = openEditor(ctx, path, "ada");

        ctx->ItemInputValue("**/##csv_filter", "id >= 2 AND name <> 'linus'");
        ctx->KeyPress(ImGuiKey_Enter);
        for (int i = 0; i < 300 && ctx->ItemExists("**/ada"); ++i)
            ctx->Yield();
        IM_CHECK(!ctx->ItemExists("**/ada"));
        IM_CHECK(ctx->ItemExists("**/grace"));
        IM_CHECK(!ctx->ItemExists("**/linus"));

        // an edit in the filtered view lands on the right row of the file
        ctx->ItemDoubleClick("**/grace");
        ctx->Yield(3);
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_A);
        ctx->KeyChars("hopper");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->KeyPress(ImGuiMod_Shortcut | ImGuiKey_S);
        ctx->Yield(3);
        IM_CHECK_EQ(readFile(path), std::string("id,name\n1,ada\n2,hopper\n3,linus\n"));

        tab->setOpen(false);
        ctx->Yield(3);
        std::filesystem::remove(path);
    };
}
