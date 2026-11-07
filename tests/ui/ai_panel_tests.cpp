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

    t = IM_REGISTER_TEST(engine, "Assistant", "Slash opens command picker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        openAssistantTab(ctx);
        ctx->ItemClick("**/##ai_side_text");
        ctx->KeyChars("/");
        ctx->Yield(3);

        IM_CHECK(ImGui::FindWindowByName("##ai_mention_popup") != nullptr);
    };
}

void RegisterCodeBlockTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Assistant", "Code block copy icon copies the code");
    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(520, 260), ImGuiCond_Always);
        ImGui::Begin("md_code_test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        MarkdownText::render(
            "Here:\n\n```sql\nSELECT id, name\nFROM users -- all\nWHERE id = 42;\n```", "c");
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(2);
        ImGui::SetClipboardText("");
        ctx->ItemClick("**/###md_cp_c_0"); // the first code block
        IM_CHECK_STR_EQ(ImGui::GetClipboardText(),
                        "SELECT id, name\nFROM users -- all\nWHERE id = 42;");
    };
}
