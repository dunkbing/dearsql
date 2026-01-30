// Must include epoxy BEFORE any other GL headers
#include <epoxy/gl.h>

#include "application.hpp"
#include "config.hpp"
#include "imgui_impl_opengl3.h"
#include "platform/linux_platform.hpp"
#include <iostream>

#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/x11/gdkx.h>
#endif

// Clipboard support for GTK4
static GdkClipboard* g_GtkClipboard = nullptr;
static char* g_ClipboardText = nullptr;
static bool g_ClipboardDirty = true;        // Need to fetch new content
static bool g_ClipboardReadPending = false; // Async read in progress

// Forward declarations for clipboard callbacks
static void clipboard_changed_callback(GdkClipboard* clipboard, gpointer user_data);

LinuxPlatform::LinuxPlatform(Application* app)
    : app_(app), window_(nullptr), glArea_(nullptr), headerBar_(nullptr), sidebarButton_(nullptr),
      workspaceDropdown_(nullptr), addButton_(nullptr), workspaceModel_(nullptr),
      shouldClose_(false), realized_(false), fbWidth_(1280), fbHeight_(720), mouseX_(0),
      mouseY_(0) {}

LinuxPlatform::~LinuxPlatform() {
    cleanup();
}

bool LinuxPlatform::initializeGTK(int* argc, char*** argv) {
    if (!gtk_init_check()) {
        std::cerr << "Failed to initialize GTK" << std::endl;
        return false;
    }

    // Set application name for WM_CLASS matching with .desktop file
    g_set_prgname(APP_NAME);
    g_set_application_name(APP_NAME);

    // Create main window
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 720);

    // Connect close signal
    g_signal_connect(window_, "close-request", G_CALLBACK(onClose), this);

    // Create GL area
    glArea_ = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(glArea_), 3, 3);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(glArea_), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(glArea_), FALSE);
    gtk_widget_set_hexpand(glArea_, TRUE);
    gtk_widget_set_vexpand(glArea_, TRUE);
    gtk_widget_set_focusable(glArea_, TRUE);
    gtk_widget_set_can_focus(glArea_, TRUE);

    // Connect GL area signals
    g_signal_connect(glArea_, "realize", G_CALLBACK(onRealize), this);
    g_signal_connect(glArea_, "render", G_CALLBACK(onRender), this);
    g_signal_connect(glArea_, "resize", G_CALLBACK(onResize), this);

    // Setup input handlers
    setupInputHandlers();

    // Set GL area as window child
    gtk_window_set_child(GTK_WINDOW(window_), glArea_);

    std::cout << "GTK window created successfully" << std::endl;
    return true;
}

void LinuxPlatform::setupInputHandlers() {
    // Key controller
    GtkEventController* keyController = gtk_event_controller_key_new();
    g_signal_connect(keyController, "key-pressed", G_CALLBACK(onKeyPress), this);
    g_signal_connect(keyController, "key-released", G_CALLBACK(onKeyRelease), this);
    gtk_widget_add_controller(glArea_, keyController);

    // Motion controller
    GtkEventController* motionController = gtk_event_controller_motion_new();
    g_signal_connect(motionController, "motion", G_CALLBACK(onMotionNotify), this);
    gtk_widget_add_controller(glArea_, motionController);

    // Click gesture for mouse buttons
    GtkGesture* clickGesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clickGesture), 0); // All buttons
    g_signal_connect(clickGesture, "pressed", G_CALLBACK(onButtonPress), this);
    g_signal_connect(clickGesture, "released", G_CALLBACK(onButtonRelease), this);
    gtk_widget_add_controller(glArea_, GTK_EVENT_CONTROLLER(clickGesture));

    // Scroll controller
    GtkEventController* scrollController =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scrollController, "scroll", G_CALLBACK(onScroll), this);
    gtk_widget_add_controller(glArea_, scrollController);
}

bool LinuxPlatform::initializePlatform(GLFWwindow* window) {
    // We don't use GLFW on Linux with GTK
    // This is called but we handle initialization in initializeGTK
    return true;
}

bool LinuxPlatform::initializeImGuiBackend() {
    if (!realized_) {
        return false;
    }

    ImGui_ImplOpenGL3_Init("#version 330");
    std::cout << "ImGui OpenGL backend initialized" << std::endl;
    return true;
}

void LinuxPlatform::setupTitlebar() {
    // Create header bar
    headerBar_ = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(headerBar_), TRUE);

    // Sidebar toggle button
    sidebarButton_ = gtk_button_new_from_icon_name("sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(sidebarButton_, "Toggle Sidebar");
    g_signal_connect(sidebarButton_, "clicked", G_CALLBACK(onSidebarToggle), this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar_), sidebarButton_);

    // Add connection button
    addButton_ = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(addButton_, "Add Database Connection");
    g_signal_connect(addButton_, "clicked", G_CALLBACK(onAddConnection), this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar_), addButton_);

    // Workspace dropdown
    workspaceModel_ = gtk_string_list_new(nullptr);
    workspaceDropdown_ = gtk_drop_down_new(G_LIST_MODEL(workspaceModel_), nullptr);
    gtk_widget_set_tooltip_text(workspaceDropdown_, "Select Workspace");
    g_signal_connect(workspaceDropdown_, "notify::selected", G_CALLBACK(onWorkspaceChanged), this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), workspaceDropdown_);

    gtk_window_set_titlebar(GTK_WINDOW(window_), headerBar_);

    std::cout << "GTK HeaderBar configured" << std::endl;
}

void LinuxPlatform::updateWorkspaceDropdown() {
    if (!workspaceModel_ || !workspaceDropdown_ || !app_) {
        return;
    }

    // Block signal during update
    g_signal_handlers_block_by_func(workspaceDropdown_, (gpointer)onWorkspaceChanged, this);

    // Clear existing items
    guint n = g_list_model_get_n_items(G_LIST_MODEL(workspaceModel_));
    for (guint i = 0; i < n; i++) {
        gtk_string_list_remove(workspaceModel_, 0);
    }

    // Add workspaces
    auto workspaces = app_->getWorkspaces();
    int currentWorkspaceId = app_->getCurrentWorkspaceId();
    guint selectedIndex = 0;

    for (size_t i = 0; i < workspaces.size(); i++) {
        gtk_string_list_append(workspaceModel_, workspaces[i].name.c_str());
        if (workspaces[i].id == currentWorkspaceId) {
            selectedIndex = static_cast<guint>(i);
        }
    }

    gtk_drop_down_set_selected(GTK_DROP_DOWN(workspaceDropdown_), selectedIndex);

    // Unblock signal
    g_signal_handlers_unblock_by_func(workspaceDropdown_, (gpointer)onWorkspaceChanged, this);
}

float LinuxPlatform::getTitlebarHeight() const {
    return 0.0f; // HeaderBar is outside the GL area
}

void LinuxPlatform::onSidebarToggleClicked() {
    if (app_) {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    }
}

void LinuxPlatform::cleanup() {
    // Cleanup clipboard
    if (g_GtkClipboard) {
        g_signal_handlers_disconnect_by_func(g_GtkClipboard, (gpointer)clipboard_changed_callback,
                                             nullptr);
    }
    if (g_ClipboardText) {
        g_free(g_ClipboardText);
        g_ClipboardText = nullptr;
    }
    g_GtkClipboard = nullptr;
    g_ClipboardDirty = true;
    g_ClipboardReadPending = false;

    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void LinuxPlatform::renderFrame() {
    if (glArea_ && realized_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(glArea_));
    }

    // Process GTK events
    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

void LinuxPlatform::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    std::cout << "ImGui OpenGL backend shutdown" << std::endl;
}

bool LinuxPlatform::shouldClose() const {
    return shouldClose_;
}

void LinuxPlatform::getFramebufferSize(int* width, int* height) const {
    *width = fbWidth_;
    *height = fbHeight_;
}

void LinuxPlatform::swapBuffers() {
    // GTK handles buffer swapping
}

void LinuxPlatform::pollEvents() {
    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

// Static callbacks
gboolean LinuxPlatform::onRender(GtkGLArea* area, GdkGLContext* context, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    if (!platform->realized_) {
        return FALSE;
    }

    // Clear with theme color
    bool darkTheme = platform->app_->isDarkTheme();
    glClearColor(darkTheme ? 0.110f : 0.957f, darkTheme ? 0.110f : 0.957f,
                 darkTheme ? 0.137f : 0.957f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_NewFrame();

    // Setup ImGui IO
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize =
        ImVec2(static_cast<float>(platform->fbWidth_), static_cast<float>(platform->fbHeight_));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // Update mouse position
    io.AddMousePosEvent(static_cast<float>(platform->mouseX_),
                        static_cast<float>(platform->mouseY_));

    ImGui::NewFrame();

    platform->app_->renderMainUI();

    ImGui::Render();

    glViewport(0, 0, platform->fbWidth_, platform->fbHeight_);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return TRUE;
}

static void clipboard_read_callback(GObject* source, GAsyncResult* result, gpointer user_data) {
    g_ClipboardReadPending = false;

    if (g_ClipboardText) {
        g_free(g_ClipboardText);
        g_ClipboardText = nullptr;
    }

    GError* error = nullptr;
    g_ClipboardText = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);

    if (error) {
        g_error_free(error);
    }

    g_ClipboardDirty = false;
}

static void clipboard_changed_callback(GdkClipboard* clipboard, gpointer user_data) {
    g_ClipboardDirty = true;

    // Start fetching new content immediately
    if (!g_ClipboardReadPending) {
        g_ClipboardReadPending = true;
        gdk_clipboard_read_text_async(clipboard, nullptr, clipboard_read_callback, nullptr);
    }
}

static const char* ImGui_ImplGtk_GetClipboardText(void* user_data) {
    if (!g_GtkClipboard) {
        return "";
    }

    // If content is dirty and no read pending, start one
    if (g_ClipboardDirty && !g_ClipboardReadPending) {
        g_ClipboardReadPending = true;
        gdk_clipboard_read_text_async(g_GtkClipboard, nullptr, clipboard_read_callback, nullptr);
    }

    // If a read is pending, wait for it to complete
    if (g_ClipboardReadPending) {
        GMainContext* context = g_main_context_default();
        gint64 end_time = g_get_monotonic_time() + 200000; // 200ms timeout

        while (g_ClipboardReadPending && g_get_monotonic_time() < end_time) {
            g_main_context_iteration(context, TRUE);
        }
    }

    return g_ClipboardText ? g_ClipboardText : "";
}

static void ImGui_ImplGtk_SetClipboardText(void* user_data, const char* text) {
    if (g_GtkClipboard && text) {
        gdk_clipboard_set_text(g_GtkClipboard, text);
    }
}

void LinuxPlatform::onRealize(GtkGLArea* area, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    gtk_gl_area_make_current(area);

    if (gtk_gl_area_get_error(area) != nullptr) {
        std::cerr << "Failed to initialize OpenGL context" << std::endl;
        return;
    }

    platform->realized_ = true;

    // Initialize ImGui OpenGL backend now that we have a context
    ImGui_ImplOpenGL3_Init("#version 330");

    // Setup clipboard
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(area));
    g_GtkClipboard = gdk_display_get_clipboard(display);

    // Listen for clipboard changes to pre-fetch content
    g_signal_connect(g_GtkClipboard, "changed", G_CALLBACK(clipboard_changed_callback), nullptr);

    // Fetch initial clipboard content
    g_ClipboardDirty = true;
    g_ClipboardReadPending = true;
    gdk_clipboard_read_text_async(g_GtkClipboard, nullptr, clipboard_read_callback, nullptr);

    ImGuiIO& io = ImGui::GetIO();
    io.GetClipboardTextFn = ImGui_ImplGtk_GetClipboardText;
    io.SetClipboardTextFn = ImGui_ImplGtk_SetClipboardText;

    std::cout << "OpenGL context realized" << std::endl;
    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
}

void LinuxPlatform::onResize(GtkGLArea* area, gint width, gint height, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->fbWidth_ = width;
    platform->fbHeight_ = height;
}

gboolean LinuxPlatform::onKeyPress(GtkEventControllerKey* controller, guint keyval, guint keycode,
                                   GdkModifierType state, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    platform->updateImGuiKeyMods(state);

    ImGuiKey key = platform->gtkKeyToImGuiKey(keyval);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, true);
    }

    // Handle text input - but NOT when Ctrl is pressed (for shortcuts like Ctrl+V)
    if (!(state & GDK_CONTROL_MASK)) {
        gunichar unicode = gdk_keyval_to_unicode(keyval);
        if (unicode != 0 && unicode < 0x10000) {
            io.AddInputCharacter(unicode);
        }
    }

    return io.WantCaptureKeyboard ? TRUE : FALSE;
}

gboolean LinuxPlatform::onKeyRelease(GtkEventControllerKey* controller, guint keyval, guint keycode,
                                     GdkModifierType state, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    platform->updateImGuiKeyMods(state);

    ImGuiKey key = platform->gtkKeyToImGuiKey(keyval);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, false);
    }

    return io.WantCaptureKeyboard ? TRUE : FALSE;
}

void LinuxPlatform::onMotionNotify(GtkEventControllerMotion* controller, gdouble x, gdouble y,
                                   gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->mouseX_ = x;
    platform->mouseY_ = y;

    if (platform->glArea_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->glArea_));
    }
}

void LinuxPlatform::onButtonPress(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y,
                                  gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    int imguiButton = 0;
    if (button == GDK_BUTTON_PRIMARY)
        imguiButton = 0;
    else if (button == GDK_BUTTON_SECONDARY)
        imguiButton = 1;
    else if (button == GDK_BUTTON_MIDDLE)
        imguiButton = 2;

    io.AddMouseButtonEvent(imguiButton, true);

    // Grab focus on click
    gtk_widget_grab_focus(platform->glArea_);

    if (platform->glArea_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->glArea_));
    }
}

void LinuxPlatform::onButtonRelease(GtkGestureClick* gesture, gint n_press, gdouble x, gdouble y,
                                    gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    int imguiButton = 0;
    if (button == GDK_BUTTON_PRIMARY)
        imguiButton = 0;
    else if (button == GDK_BUTTON_SECONDARY)
        imguiButton = 1;
    else if (button == GDK_BUTTON_MIDDLE)
        imguiButton = 2;

    io.AddMouseButtonEvent(imguiButton, false);

    if (platform->glArea_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->glArea_));
    }
}

gboolean LinuxPlatform::onScroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy,
                                 gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    io.AddMouseWheelEvent(static_cast<float>(-dx), static_cast<float>(-dy));

    if (platform->glArea_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->glArea_));
    }

    return TRUE;
}

void LinuxPlatform::onSidebarToggle(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->onSidebarToggleClicked();
}

void LinuxPlatform::onWorkspaceChanged(GtkDropDown* dropdown, GParamSpec* pspec,
                                       gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    if (!platform->app_)
        return;

    guint selected = gtk_drop_down_get_selected(dropdown);
    auto workspaces = platform->app_->getWorkspaces();

    if (selected < workspaces.size()) {
        platform->app_->setCurrentWorkspace(workspaces[selected].id);
    }
}

void LinuxPlatform::onAddConnection(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    if (platform->app_ && platform->app_->getDatabaseSidebar()) {
        platform->app_->getDatabaseSidebar()->showConnectionDialog();
    }
}

gboolean LinuxPlatform::onClose(GtkWindow* window, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->shouldClose_ = true;
    return FALSE;
}

void LinuxPlatform::updateImGuiKeyMods(GdkModifierType state) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (state & GDK_CONTROL_MASK) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (state & GDK_SHIFT_MASK) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (state & GDK_ALT_MASK) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (state & GDK_SUPER_MASK) != 0);
}

ImGuiKey LinuxPlatform::gtkKeyToImGuiKey(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Tab:
        return ImGuiKey_Tab;
    case GDK_KEY_Left:
        return ImGuiKey_LeftArrow;
    case GDK_KEY_Right:
        return ImGuiKey_RightArrow;
    case GDK_KEY_Up:
        return ImGuiKey_UpArrow;
    case GDK_KEY_Down:
        return ImGuiKey_DownArrow;
    case GDK_KEY_Page_Up:
        return ImGuiKey_PageUp;
    case GDK_KEY_Page_Down:
        return ImGuiKey_PageDown;
    case GDK_KEY_Home:
        return ImGuiKey_Home;
    case GDK_KEY_End:
        return ImGuiKey_End;
    case GDK_KEY_Insert:
        return ImGuiKey_Insert;
    case GDK_KEY_Delete:
        return ImGuiKey_Delete;
    case GDK_KEY_BackSpace:
        return ImGuiKey_Backspace;
    case GDK_KEY_space:
        return ImGuiKey_Space;
    case GDK_KEY_Return:
        return ImGuiKey_Enter;
    case GDK_KEY_Escape:
        return ImGuiKey_Escape;
    case GDK_KEY_apostrophe:
        return ImGuiKey_Apostrophe;
    case GDK_KEY_comma:
        return ImGuiKey_Comma;
    case GDK_KEY_minus:
        return ImGuiKey_Minus;
    case GDK_KEY_period:
        return ImGuiKey_Period;
    case GDK_KEY_slash:
        return ImGuiKey_Slash;
    case GDK_KEY_semicolon:
        return ImGuiKey_Semicolon;
    case GDK_KEY_equal:
        return ImGuiKey_Equal;
    case GDK_KEY_bracketleft:
        return ImGuiKey_LeftBracket;
    case GDK_KEY_backslash:
        return ImGuiKey_Backslash;
    case GDK_KEY_bracketright:
        return ImGuiKey_RightBracket;
    case GDK_KEY_grave:
        return ImGuiKey_GraveAccent;
    case GDK_KEY_Caps_Lock:
        return ImGuiKey_CapsLock;
    case GDK_KEY_Scroll_Lock:
        return ImGuiKey_ScrollLock;
    case GDK_KEY_Num_Lock:
        return ImGuiKey_NumLock;
    case GDK_KEY_Print:
        return ImGuiKey_PrintScreen;
    case GDK_KEY_Pause:
        return ImGuiKey_Pause;
    case GDK_KEY_KP_0:
        return ImGuiKey_Keypad0;
    case GDK_KEY_KP_1:
        return ImGuiKey_Keypad1;
    case GDK_KEY_KP_2:
        return ImGuiKey_Keypad2;
    case GDK_KEY_KP_3:
        return ImGuiKey_Keypad3;
    case GDK_KEY_KP_4:
        return ImGuiKey_Keypad4;
    case GDK_KEY_KP_5:
        return ImGuiKey_Keypad5;
    case GDK_KEY_KP_6:
        return ImGuiKey_Keypad6;
    case GDK_KEY_KP_7:
        return ImGuiKey_Keypad7;
    case GDK_KEY_KP_8:
        return ImGuiKey_Keypad8;
    case GDK_KEY_KP_9:
        return ImGuiKey_Keypad9;
    case GDK_KEY_KP_Decimal:
        return ImGuiKey_KeypadDecimal;
    case GDK_KEY_KP_Divide:
        return ImGuiKey_KeypadDivide;
    case GDK_KEY_KP_Multiply:
        return ImGuiKey_KeypadMultiply;
    case GDK_KEY_KP_Subtract:
        return ImGuiKey_KeypadSubtract;
    case GDK_KEY_KP_Add:
        return ImGuiKey_KeypadAdd;
    case GDK_KEY_KP_Enter:
        return ImGuiKey_KeypadEnter;
    case GDK_KEY_KP_Equal:
        return ImGuiKey_KeypadEqual;
    case GDK_KEY_Shift_L:
        return ImGuiKey_LeftShift;
    case GDK_KEY_Control_L:
        return ImGuiKey_LeftCtrl;
    case GDK_KEY_Alt_L:
        return ImGuiKey_LeftAlt;
    case GDK_KEY_Super_L:
        return ImGuiKey_LeftSuper;
    case GDK_KEY_Shift_R:
        return ImGuiKey_RightShift;
    case GDK_KEY_Control_R:
        return ImGuiKey_RightCtrl;
    case GDK_KEY_Alt_R:
        return ImGuiKey_RightAlt;
    case GDK_KEY_Super_R:
        return ImGuiKey_RightSuper;
    case GDK_KEY_Menu:
        return ImGuiKey_Menu;
    case GDK_KEY_0:
        return ImGuiKey_0;
    case GDK_KEY_1:
        return ImGuiKey_1;
    case GDK_KEY_2:
        return ImGuiKey_2;
    case GDK_KEY_3:
        return ImGuiKey_3;
    case GDK_KEY_4:
        return ImGuiKey_4;
    case GDK_KEY_5:
        return ImGuiKey_5;
    case GDK_KEY_6:
        return ImGuiKey_6;
    case GDK_KEY_7:
        return ImGuiKey_7;
    case GDK_KEY_8:
        return ImGuiKey_8;
    case GDK_KEY_9:
        return ImGuiKey_9;
    case GDK_KEY_a:
    case GDK_KEY_A:
        return ImGuiKey_A;
    case GDK_KEY_b:
    case GDK_KEY_B:
        return ImGuiKey_B;
    case GDK_KEY_c:
    case GDK_KEY_C:
        return ImGuiKey_C;
    case GDK_KEY_d:
    case GDK_KEY_D:
        return ImGuiKey_D;
    case GDK_KEY_e:
    case GDK_KEY_E:
        return ImGuiKey_E;
    case GDK_KEY_f:
    case GDK_KEY_F:
        return ImGuiKey_F;
    case GDK_KEY_g:
    case GDK_KEY_G:
        return ImGuiKey_G;
    case GDK_KEY_h:
    case GDK_KEY_H:
        return ImGuiKey_H;
    case GDK_KEY_i:
    case GDK_KEY_I:
        return ImGuiKey_I;
    case GDK_KEY_j:
    case GDK_KEY_J:
        return ImGuiKey_J;
    case GDK_KEY_k:
    case GDK_KEY_K:
        return ImGuiKey_K;
    case GDK_KEY_l:
    case GDK_KEY_L:
        return ImGuiKey_L;
    case GDK_KEY_m:
    case GDK_KEY_M:
        return ImGuiKey_M;
    case GDK_KEY_n:
    case GDK_KEY_N:
        return ImGuiKey_N;
    case GDK_KEY_o:
    case GDK_KEY_O:
        return ImGuiKey_O;
    case GDK_KEY_p:
    case GDK_KEY_P:
        return ImGuiKey_P;
    case GDK_KEY_q:
    case GDK_KEY_Q:
        return ImGuiKey_Q;
    case GDK_KEY_r:
    case GDK_KEY_R:
        return ImGuiKey_R;
    case GDK_KEY_s:
    case GDK_KEY_S:
        return ImGuiKey_S;
    case GDK_KEY_t:
    case GDK_KEY_T:
        return ImGuiKey_T;
    case GDK_KEY_u:
    case GDK_KEY_U:
        return ImGuiKey_U;
    case GDK_KEY_v:
    case GDK_KEY_V:
        return ImGuiKey_V;
    case GDK_KEY_w:
    case GDK_KEY_W:
        return ImGuiKey_W;
    case GDK_KEY_x:
    case GDK_KEY_X:
        return ImGuiKey_X;
    case GDK_KEY_y:
    case GDK_KEY_Y:
        return ImGuiKey_Y;
    case GDK_KEY_z:
    case GDK_KEY_Z:
        return ImGuiKey_Z;
    case GDK_KEY_F1:
        return ImGuiKey_F1;
    case GDK_KEY_F2:
        return ImGuiKey_F2;
    case GDK_KEY_F3:
        return ImGuiKey_F3;
    case GDK_KEY_F4:
        return ImGuiKey_F4;
    case GDK_KEY_F5:
        return ImGuiKey_F5;
    case GDK_KEY_F6:
        return ImGuiKey_F6;
    case GDK_KEY_F7:
        return ImGuiKey_F7;
    case GDK_KEY_F8:
        return ImGuiKey_F8;
    case GDK_KEY_F9:
        return ImGuiKey_F9;
    case GDK_KEY_F10:
        return ImGuiKey_F10;
    case GDK_KEY_F11:
        return ImGuiKey_F11;
    case GDK_KEY_F12:
        return ImGuiKey_F12;
    default:
        return ImGuiKey_None;
    }
}

void LinuxPlatform::runMainLoop() {
    gtk_window_present(GTK_WINDOW(window_));

    while (!shouldClose_) {
        // Process GTK events
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, FALSE);
        }

        // Request redraw
        if (glArea_ && realized_) {
            gtk_gl_area_queue_render(GTK_GL_AREA(glArea_));
        }

        // Small sleep to prevent 100% CPU usage
        g_usleep(1000); // 1ms
    }
}
