#include "application.hpp"
#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "imgui_te_ui.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>

// test registration, one function per area
void RegisterSidebarTests(ImGuiTestEngine* engine);
void RegisterAiPanelTests(ImGuiTestEngine* engine);

int main(int argc, char** argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-nopause") == 0) {
            headless = true;
        }
    }

    auto& app = Application::getInstance();
    if (!app.initialize()) {
        std::fprintf(stderr, "ui_tests: app failed to initialize\n");
        return 1;
    }

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr) {
        std::fprintf(stderr, "ui_tests: no imgui context\n");
        return 1;
    }

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& testIo = ImGuiTestEngine_GetIO(engine);
    testIo.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    testIo.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    testIo.ConfigRunSpeed = headless ? ImGuiTestRunSpeed_Fast : ImGuiTestRunSpeed_Normal;
    testIo.ConfigRestoreFocusAfterTests = false;
    testIo.ConfigLogToTTY = true; // otherwise failures never reach stdout

    ImGuiTestEngine_Start(engine, ctx);

    RegisterSidebarTests(engine);
    RegisterAiPanelTests(engine);

    if (headless) {
        ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, nullptr);
    }

    // Drive the platform's own frame instead of hand-rolling one: on macOS the
    // frame is a Metal pass (drawable, encoder, backend NewFrame) that the app
    // owns, and renderFrame() is what calls renderMainUI().
    PlatformInterface* platform = app.getPlatform();
    while (!glfwWindowShouldClose(app.getWindow())) {
        glfwPollEvents();
        platform->renderFrame();
        ImGuiTestEngine_PostSwap(engine);

        if (headless && ImGuiTestEngine_IsTestQueueEmpty(engine)) {
            break;
        }
    }

    ImGuiTestEngine_Stop(engine);

    ImGuiTestEngineResultSummary summary;
    ImGuiTestEngine_GetResultSummary(engine, &summary);
    std::printf("\n=== ui_tests ===\ntested: %d\npassed: %d\nfailed: %d\n", summary.CountTested,
                summary.CountSuccess, summary.CountTested - summary.CountSuccess);

    // cleanup() destroys the imgui context, which the engine requires to happen first
    app.cleanup();
    ImGuiTestEngine_DestroyContext(engine);

    return summary.CountTested != summary.CountSuccess ? 1 : 0;
}
