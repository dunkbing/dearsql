# Dear SQL UI Tests

This directory contains UI automation tests using [ImGui Test Engine](https://github.com/ocornut/imgui_test_engine).

## Overview

The tests are built on ImGui Test Engine, which provides automated UI testing for ImGui applications. Tests simulate user interactions with the actual UI and verify behavior.

## Running Tests

### Quick Start

```bash
# Run tests with interactive GUI (see test engine window)
./test

# Run tests in headless mode (for CI/CD)
./test headless
```

### Manual Build & Run

```bash
# Build tests
cd build
cmake --build . --target ui_tests

# Run interactively
./ui_tests

# Run headless
./ui_tests -nopause
```

## Test Structure

### Test Files

- **`tests/ui/main_test.cpp`**: Test runner entry point
  - Initializes the application
  - Creates and configures ImGui Test Engine
  - Registers all test suites
  - Manages test execution loop
  - Reports results

- **`tests/ui/sidebar_tests.cpp`**: Sidebar UI tests
  - Sidebar visibility toggling
  - Database list display
  - Database selection
  - Tree node expansion
  - SQLite connection dialog
  - Multiple database handling
  - Workspace switching
  - Rendering stability

### Adding New Tests

To add a new test, register it in the appropriate test file:

```cpp
void RegisterMyTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    t = IM_REGISTER_TEST(engine, "Category", "Test Name");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        // Render your UI here
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Test logic here
        auto& app = Application::getInstance();

        // Simulate user actions
        ctx->Yield(); // Wait one frame

        // Check conditions
        IM_CHECK(app.isSidebarVisible() == true);
    };
}
```

Then register your test suite in `main_test.cpp`:

```cpp
// Add forward declaration
void RegisterMyTests(ImGuiTestEngine* engine);

// In main()
RegisterSidebarTests(engine);
RegisterMyTests(engine);  // Add this line
```

## Test API Reference

### Common Operations

```cpp
// Wait for frames
ctx->Yield();       // Wait 1 frame
ctx->Yield(5);      // Wait 5 frames

// Find and interact with UI elements
ctx->ItemClick("*/Button Label");
ctx->ItemOpen("*/TreeNode");
ctx->ItemClose("*/TreeNode");
ctx->ItemInput("*/InputField");

// Assertions
IM_CHECK(condition);
IM_CHECK_EQ(a, b);
IM_CHECK_NE(a, b);
```

### ImGui Test Engine Documentation

For more details on ImGui Test Engine API:
- [Official Wiki](https://github.com/ocornut/imgui_test_engine/wiki)
- See `external/imgui_test_engine/docs/` for documentation

## Current Test Coverage

### Sidebar Tests
- ✅ Toggle visibility
- ✅ Show database list
- ✅ Select database
- ✅ Expand tree nodes
- ✅ SQLite connection dialog workflow
- ✅ Multiple databases
- ✅ Workspace switching
- ✅ Basic rendering stability

### Future Test Ideas
- SQL editor tab interactions
- Table viewer functionality
- Query execution
- ER diagram rendering
- Tab management
- Connection persistence

## CI/CD Integration

The tests can run in headless mode for continuous integration:

```bash
#!/bin/bash
# Example CI script
./test headless
exit_code=$?

if [ $exit_code -eq 0 ]; then
    echo "✓ All tests passed!"
else
    echo "✗ Tests failed"
    exit 1
fi
```

## Troubleshooting

### Tests fail to build
- Ensure imgui_test_engine submodule is initialized: `git submodule update --init --recursive`
- Check CMake version is 3.23+
- Verify all dependencies are installed

### Tests crash on startup
- Check that the application initializes correctly
- Verify ImGui context is created before test engine
- Look for error messages in console output

### Tests fail intermittently
- Increase `ctx->Yield()` waits for async operations
- Check for race conditions in async code
- Verify test isolation (each test should clean up)

## Notes

- Tests run within the actual application context
- All application functionality is available during tests
- Tests use the real ImGui rendering pipeline
- Perfect for integration/E2E testing
- Can run with or without visible GUI
