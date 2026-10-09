#include "application.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "ui/markdown_text.hpp"
#include "ui/tab_manager.hpp"

// Tests for the assistant chat tab. These drive the real widgets by
// id, so they cover the things a build cannot: that the tab renders, that the
// input exists, and that typing '@' opens the context picker.
namespace {
    // api backend spawns no agent, mcp server or registry fetch under test
    void openAssistantTab(ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.getAppState()->setSetting("ai_sidebar_backend", "api");
        // a chat tab normally opens from a connection's context menu; unbound works too
        const auto tab = app.getTabManager()->createAIChatTab(nullptr);
        ctx->Yield(2);
        ctx->SetRef(tab->getWindowName().c_str());
        ctx->Yield(2);
    }
} // namespace

void RegisterAiPanelTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    t = IM_REGISTER_TEST(engine, "Assistant", "Open tab");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        // the input only exists once the panel is up
        IM_CHECK(ctx->ItemExists("**/##ai_side_text"));
    };

    t = IM_REGISTER_TEST(engine, "Assistant", "At sign opens context picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("@");
        ctx->Yield(3);

        // the picker is a window of its own, opened only while '@' is pending
        IM_CHECK(ImGui::FindWindowByName("##ai_mention_popup") != nullptr);
    };

    t = IM_REGISTER_TEST(engine, "Assistant", "Session picker opens");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/###ai_sessions");
        ctx->Yield(3);

        // BeginPopup windows get internal names, so check the popup stack
        IM_CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size > 0);
        // any click outside closes it; the engine will not hover items under a popup,
        // so aim at a raw position
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ctx->MouseMoveToPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f));
        ctx->MouseClick(ImGuiMouseButton_Left);
        ctx->Yield(2);
        IM_CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
    };

    t = IM_REGISTER_TEST(engine, "Assistant", "Slash opens command picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("/");
        ctx->Yield(3);

        IM_CHECK(ImGui::FindWindowByName("##ai_mention_popup") != nullptr);
    };
}

void RegisterMarkdownTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Assistant", "Markdown table renders as a table");
    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::Begin("md_test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        MarkdownText::render("| name | rows |\n|---|--:|\n| users | 42 |\n| orders | 7 |", "t");
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(2);
        ImGuiWindow* window = ImGui::FindWindowByName("md_test");
        IM_CHECK(window != nullptr);
        // block 0 is the table, so its id is ##md_table_t_0
        ImGuiTable* table = ImGui::TableFindByID(window->GetID("##md_table_t_0"));
        IM_CHECK(table != nullptr);
        IM_CHECK_EQ(table->ColumnsCount, 2);
    };
}
