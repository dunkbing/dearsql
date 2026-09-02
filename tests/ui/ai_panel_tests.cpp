#include "application.hpp"
#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// Tests for the Assistant tab in the sidebar. These drive the real widgets by
// id, so they cover the things a build cannot: that the tab renders, that the
// input exists, and that typing '@' opens the context picker.
namespace {
    // api backend spawns no agent, mcp server or registry fetch under test
    void openAssistantTab(ImGuiTestContext* ctx) {
        auto& app = Application::getInstance();
        app.getAppState()->setSetting("ai_sidebar_backend", "api");
        app.setSidebarVisible(true);
        ctx->Yield(2);

        ctx->SetRef("Databases");
        ctx->ItemClick("**/##sidebar_tab_1"); // rotated "Assistant" strip button, inside a child
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

    t = IM_REGISTER_TEST(engine, "Assistant", "Slash opens command picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("/");
        ctx->Yield(3);

        IM_CHECK(ImGui::FindWindowByName("##ai_mention_popup") != nullptr);
    };
}
