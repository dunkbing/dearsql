#include "application.hpp"
#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_ui.h"
#include <GLFW/glfw3.h>

#include "imgui_impl_glfw.h"

// forward declarations for test registration
void RegisterSidebarTests(ImGuiTestEngine* engine);

int main(int argc, char** argv) {
    auto& app = Application::getInstance();

    if (!app.initialize()) {
        return -1;
    }

    // get ImGui context
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) {
        return -1;
    }

    // create and configure test engine
    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(engine);

    // configure test engine
    test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    test_io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;

    // parse command line arguments
    bool no_gui = false;
    if (argc >= 2) {
        if (strcmp(argv[1], "-nopause") == 0) {
            no_gui = true;
        }
    }

    // initialize test engine
    ImGuiTestEngine_Start(engine, ctx);

    // register all tests
    RegisterSidebarTests(engine);

    // queue all tests to run
    if (no_gui) {
        ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, nullptr);
    }

    // main loop
    bool aborted = false;
    while (!aborted) {
        glfwPollEvents();

        if (glfwWindowShouldClose(app.getWindow())) {
            aborted = true;
        }

        if (aborted && ImGuiTestEngine_TryAbortEngine(engine)) {
            break;
        }

        // start new ImGui frame
        // call backend NewFrame functions to set up display size and other state
#ifdef __APPLE__
        // note: on macOS we can't actually render without a real Metal render pass descriptor
        // but we can still call the ImGui frame functions for UI logic testing
        ImGui_ImplGlfw_NewFrame();
#else
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
#endif
        ImGui::NewFrame();

        // render main application UI
        app.renderMainUI();

        // render test engine UI
        if (!no_gui) {
            ImGuiTestEngine_ShowTestEngineWindows(engine, nullptr);
        }

        // end frame
        ImGui::Render();

        // post-swap for test engine (required for screen capture)
        ImGuiTestEngine_PostSwap(engine);

        // check if tests are done (for headless mode)
        if (no_gui && ImGuiTestEngine_IsTestQueueEmpty(engine)) {
            // all tests are done, we can exit
            break;
        }
    }

    // stop test engine
    ImGuiTestEngine_Stop(engine);

    // get and print results
    ImGuiTestEngineResultSummary summary;
    ImGuiTestEngine_GetResultSummary(engine, &summary);

    printf("\n=== Test Results ===\n");
    printf("Tested: %d\n", summary.CountTested);
    printf("Success: %d\n", summary.CountSuccess);
    printf("Failed: %d\n", summary.CountTested - summary.CountSuccess);
    printf("In Queue: %d\n", summary.CountInQueue);

    // cleanup
    ImGuiTestEngine_DestroyContext(engine);
    app.cleanup();

    return (summary.CountTested != summary.CountSuccess) ? 1 : 0;
}
