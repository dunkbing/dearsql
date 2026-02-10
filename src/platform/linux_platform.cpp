// Must include epoxy BEFORE any other GL headers
#include <epoxy/gl.h>

#include "application.hpp"
#include "config.hpp"
#include "imgui_impl_opengl3.h"
#include "license/license_manager.hpp"
#include "platform/linux_platform.hpp"
#include "themes.hpp"
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
      workspaceDropdown_(nullptr), addButton_(nullptr), menuButton_(nullptr), menuPopover_(nullptr),
      themeLightButton_(nullptr), themeDarkButton_(nullptr), themeAutoButton_(nullptr),
      licenseButton_(nullptr), workspaceModel_(nullptr), shouldClose_(false), realized_(false),
      fbWidth_(1280), fbHeight_(720), mouseX_(0), mouseY_(0) {}

LinuxPlatform::~LinuxPlatform() {
    cleanup();
}

bool LinuxPlatform::initializeGTK(int* argc, char*** argv) {
    if (!gtk_init_check()) {
        std::cerr << "Failed to initialize GTK" << std::endl;
        return false;
    }

    // Set runtime application ID for desktop entry / dock matching
    g_set_prgname(APP_IDENTIFIER);
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

    // Workspace dropdown (packed later, after menu button)
    workspaceModel_ = gtk_string_list_new(nullptr);
    workspaceDropdown_ = gtk_drop_down_new(G_LIST_MODEL(workspaceModel_), nullptr);
    gtk_widget_set_tooltip_text(workspaceDropdown_, "Select Workspace");
    workspaceSignalId_ = g_signal_connect(workspaceDropdown_, "notify::selected",
                                          G_CALLBACK(onWorkspaceChanged), this);

    // Main menu button (hamburger menu)
    menuButton_ = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton_), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menuButton_, "Main Menu");

    // Create popover content
    menuPopover_ = gtk_popover_new();
    GtkWidget* menuBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(menuBox, 12);
    gtk_widget_set_margin_end(menuBox, 12);
    gtk_widget_set_margin_top(menuBox, 12);
    gtk_widget_set_margin_bottom(menuBox, 12);
    gtk_widget_set_size_request(menuBox, 180, -1);

    // Theme section
    GtkWidget* themeLabel = gtk_label_new("Theme");
    gtk_widget_set_halign(themeLabel, GTK_ALIGN_START);
    gtk_widget_add_css_class(themeLabel, "dim-label");
    gtk_box_append(GTK_BOX(menuBox), themeLabel);

    // Theme buttons in a horizontal box with linked style
    GtkWidget* themeButtonBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(themeButtonBox, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(themeButtonBox, "linked");

    themeLightButton_ = gtk_button_new_from_icon_name("weather-clear-symbolic");
    gtk_widget_set_tooltip_text(themeLightButton_, "Light");
    gtk_widget_set_hexpand(themeLightButton_, TRUE);
    g_signal_connect(themeLightButton_, "clicked", G_CALLBACK(onThemeLightClicked), this);
    gtk_box_append(GTK_BOX(themeButtonBox), themeLightButton_);

    themeDarkButton_ = gtk_button_new_from_icon_name("weather-clear-night-symbolic");
    gtk_widget_set_tooltip_text(themeDarkButton_, "Dark");
    gtk_widget_set_hexpand(themeDarkButton_, TRUE);
    g_signal_connect(themeDarkButton_, "clicked", G_CALLBACK(onThemeDarkClicked), this);
    gtk_box_append(GTK_BOX(themeButtonBox), themeDarkButton_);

    themeAutoButton_ = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(themeAutoButton_, "System");
    gtk_widget_set_hexpand(themeAutoButton_, TRUE);
    g_signal_connect(themeAutoButton_, "clicked", G_CALLBACK(onThemeAutoClicked), this);
    gtk_box_append(GTK_BOX(themeButtonBox), themeAutoButton_);

    gtk_box_append(GTK_BOX(menuBox), themeButtonBox);

    // Separator
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(menuBox), separator);

    // License button
    licenseButton_ = gtk_button_new_with_label("Manage License...");
    gtk_widget_set_halign(licenseButton_, GTK_ALIGN_FILL);
    g_signal_connect(licenseButton_, "clicked", G_CALLBACK(onLicenseClicked), this);
    gtk_box_append(GTK_BOX(menuBox), licenseButton_);

    // Report Bug button
    GtkWidget* reportBugButton = gtk_button_new_with_label("Report Bug...");
    gtk_widget_set_halign(reportBugButton, GTK_ALIGN_FILL);
    g_signal_connect(reportBugButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userData) {
                         auto* platform = static_cast<LinuxPlatform*>(userData);
                         gtk_popover_popdown(GTK_POPOVER(platform->menuPopover_));
                         std::string url =
                             "https://github.com/dunkbing/dearsql-website/issues/new?labels=bug"
                             "&title=%5BBug%5D+&body=%23%23+Description%0A%0A%23%23+Steps+"
                             "to+Reproduce%0A1.+%0A2.+%0A3.+%0A%0A%23%23+Expected+Behavior"
                             "%0A%0A%23%23+Actual+Behavior%0A%0A%23%23+Environment%0A-+**OS"
                             "**%3A+Linux%0A-+**DearSQL+version**%3A+" APP_VERSION
                             "%0A-+**Database**%3A+";
                         GtkUriLauncher* launcher = gtk_uri_launcher_new(url.c_str());
                         gtk_uri_launcher_launch(launcher, GTK_WINDOW(platform->window_),
                                                 nullptr, nullptr, nullptr);
                         g_object_unref(launcher);
                     }),
                     this);
    gtk_box_append(GTK_BOX(menuBox), reportBugButton);

    gtk_popover_set_child(GTK_POPOVER(menuPopover_), menuBox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menuButton_), menuPopover_);

    // Update theme and license button on popover show
    g_signal_connect(menuPopover_, "show", G_CALLBACK(+[](GtkWidget*, gpointer userData) {
                         auto* platform = static_cast<LinuxPlatform*>(userData);
                         platform->updateThemeButtons();
                         platform->updateLicenseButton();
                     }),
                     this);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), menuButton_);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), workspaceDropdown_);

    gtk_window_set_titlebar(GTK_WINDOW(window_), headerBar_);
    updateGtkTheme();

    std::cout << "GTK HeaderBar configured" << std::endl;
}

void LinuxPlatform::updateWorkspaceDropdown() {
    if (!workspaceModel_ || !workspaceDropdown_ || !app_) {
        return;
    }

    // Disconnect signal to prevent spurious notifications during model rebuild
    if (workspaceSignalId_) {
        g_signal_handler_disconnect(workspaceDropdown_, workspaceSignalId_);
        workspaceSignalId_ = 0;
    }

    // Clear existing items
    guint n = g_list_model_get_n_items(G_LIST_MODEL(workspaceModel_));
    for (guint i = 0; i < n; i++) {
        gtk_string_list_remove(workspaceModel_, 0);
    }
    workspaceIdsByIndex_.clear();

    // Add workspaces
    auto workspaces = app_->getWorkspaces();
    int currentWorkspaceId = app_->getCurrentWorkspaceId();
    guint selectedIndex = 0;

    for (size_t i = 0; i < workspaces.size(); i++) {
        gtk_string_list_append(workspaceModel_, workspaces[i].name.c_str());
        workspaceIdsByIndex_.push_back(workspaces[i].id);
        if (workspaces[i].id == currentWorkspaceId) {
            selectedIndex = static_cast<guint>(i);
        }
    }

    // Add "New Workspace..." option at the end
    gtk_string_list_append(workspaceModel_, "New Workspace...");

    gtk_drop_down_set_selected(GTK_DROP_DOWN(workspaceDropdown_), selectedIndex);

    // Reconnect signal after model is stable
    workspaceSignalId_ = g_signal_connect(workspaceDropdown_, "notify::selected",
                                          G_CALLBACK(onWorkspaceChanged), this);
}

void LinuxPlatform::applyCurrentTheme() {
    updateGtkTheme();
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
    if (selected == GTK_INVALID_LIST_POSITION)
        return;

    if (selected < platform->workspaceIdsByIndex_.size()) {
        int workspaceId = platform->workspaceIdsByIndex_[selected];

        // Disconnect signal before calling setCurrentWorkspace to prevent
        // any re-entrant signals from reverting the change
        if (platform->workspaceSignalId_) {
            g_signal_handler_disconnect(dropdown, platform->workspaceSignalId_);
            platform->workspaceSignalId_ = 0;
        }

        platform->app_->setCurrentWorkspace(workspaceId);

        // Reconnect after workspace change is fully applied
        platform->workspaceSignalId_ = g_signal_connect(dropdown, "notify::selected",
                                                        G_CALLBACK(onWorkspaceChanged), platform);
    } else {
        // "New Workspace..." selected - revert dropdown and show dialog
        platform->updateWorkspaceDropdown();
        platform->showCreateWorkspaceDialog();
    }
}

void LinuxPlatform::onAddConnection(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    if (platform->app_ && platform->app_->getDatabaseSidebar()) {
        platform->app_->getDatabaseSidebar()->showConnectionDialog();
    }
}

void LinuxPlatform::showCreateWorkspaceDialog() {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Create New Workspace");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);

    GtkWidget* label = gtk_label_new("Enter a name for the new workspace:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Workspace name");
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget* buttonBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttonBox, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttonBox, 8);

    GtkWidget* cancelButton = gtk_button_new_with_label("Cancel");
    GtkWidget* createButton = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(createButton, "suggested-action");

    gtk_box_append(GTK_BOX(buttonBox), cancelButton);
    gtk_box_append(GTK_BOX(buttonBox), createButton);
    gtk_box_append(GTK_BOX(box), buttonBox);

    g_object_set_data(G_OBJECT(dialog), "entry", entry);
    g_object_set_data(G_OBJECT(dialog), "platform", this);

    g_signal_connect(cancelButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer dlg) {
                         auto* platform = static_cast<LinuxPlatform*>(
                             g_object_get_data(G_OBJECT(dlg), "platform"));
                         // Revert dropdown to current workspace
                         platform->updateWorkspaceDropdown();
                         gtk_window_destroy(GTK_WINDOW(dlg));
                     }),
                     dialog);

    g_signal_connect(dialog, "close-request", G_CALLBACK(+[](GtkWindow* win, gpointer) -> gboolean {
                         auto* platform = static_cast<LinuxPlatform*>(
                             g_object_get_data(G_OBJECT(win), "platform"));
                         platform->updateWorkspaceDropdown();
                         return FALSE;
                     }),
                     nullptr);

    g_signal_connect(createButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer dlg) {
                         auto* platform = static_cast<LinuxPlatform*>(
                             g_object_get_data(G_OBJECT(dlg), "platform"));
                         GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "entry"));

                         const char* name = gtk_editable_get_text(GTK_EDITABLE(entry));
                         if (name && strlen(name) > 0 && platform->app_) {
                             int newId = platform->app_->createWorkspace(std::string(name));
                             if (newId > 0) {
                                 platform->updateWorkspaceDropdown();
                             }
                         }
                         gtk_window_destroy(GTK_WINDOW(dlg));
                     }),
                     dialog);

    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

gboolean LinuxPlatform::onClose(GtkWindow* window, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->shouldClose_ = true;
    return FALSE;
}

void LinuxPlatform::updateThemeButtons() {
    if (!app_ || !themeLightButton_ || !themeDarkButton_ || !themeAutoButton_) {
        return;
    }

    bool isDark = app_->isDarkTheme();

    // Remove suggested-action from all buttons first
    gtk_widget_remove_css_class(themeLightButton_, "suggested-action");
    gtk_widget_remove_css_class(themeDarkButton_, "suggested-action");
    gtk_widget_remove_css_class(themeAutoButton_, "suggested-action");

    // Add suggested-action to the currently selected theme
    if (isDark) {
        gtk_widget_add_css_class(themeDarkButton_, "suggested-action");
    } else {
        gtk_widget_add_css_class(themeLightButton_, "suggested-action");
    }
}

void LinuxPlatform::updateGtkTheme() {
    if (!app_ || !window_)
        return;

    bool isDark = app_->isDarkTheme();

    GtkSettings* settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-application-prefer-dark-theme", isDark ? TRUE : FALSE, nullptr);

    static GtkCssProvider* themeProvider = nullptr;
    if (!themeProvider) {
        themeProvider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(themeProvider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    const auto& colors = isDark ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    auto toHex = [](const ImVec4& c) -> std::string {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", (int)(c.x * 255), (int)(c.y * 255),
                 (int)(c.z * 255));
        return buf;
    };

    std::string mantle = toHex(colors.mantle);
    std::string base = toHex(colors.base);
    std::string text = toHex(colors.text);
    std::string surface0 = toHex(colors.surface0);
    std::string surface1 = toHex(colors.surface1);
    std::string overlay0 = toHex(colors.overlay0);

    std::string css = "window { background: " + base +
                      "; }\n"

                      /* headerbar background and text */
                      "headerbar { background: " +
                      mantle + "; color: " + text +
                      "; }\n"

                      /* headerbar buttons (flat style) */
                      "headerbar button:not(.titlebutton) {"
                      "  color: " +
                      text +
                      ";"
                      "  background: transparent;"
                      "  border-color: transparent;"
                      "  box-shadow: none;"
                      "}\n"
                      "headerbar button:not(.titlebutton):hover {"
                      "  background: " +
                      surface0 +
                      ";"
                      "}\n"
                      "headerbar button:not(.titlebutton):active,"
                      "headerbar button:not(.titlebutton):checked {"
                      "  background: " +
                      surface1 +
                      ";"
                      "}\n"

                      /* icons inside headerbar buttons */
                      "headerbar button image { color: " +
                      text +
                      "; -gtk-icon-filter: none; }\n"

                      /* dropdown in headerbar */
                      "headerbar dropdown > button {"
                      "  color: " +
                      text +
                      ";"
                      "  background: " +
                      surface0 +
                      ";"
                      "  border: 1px solid " +
                      overlay0 +
                      ";"
                      "}\n"
                      "headerbar dropdown > button:hover {"
                      "  background: " +
                      surface1 +
                      ";"
                      "}\n"

                      /* window control buttons (min/max/close) */
                      "headerbar windowcontrols button.titlebutton {"
                      "  color: " +
                      text +
                      ";"
                      "}\n"
                      "headerbar windowcontrols button.titlebutton image {"
                      "  color: " +
                      text +
                      ";"
                      "  -gtk-icon-filter: none;"
                      "}\n"

                      /* popover background and arrow */
                      "popover > contents {"
                      "  background: " +
                      surface0 +
                      ";"
                      "  color: " +
                      text +
                      ";"
                      "}\n"
                      "popover > arrow {"
                      "  background: " +
                      surface0 +
                      ";"
                      "}\n"
                      "popover label { color: " +
                      text +
                      "; }\n"
                      "popover button {"
                      "  color: " +
                      text +
                      ";"
                      "  background: transparent;"
                      "  border-color: " +
                      overlay0 +
                      ";"
                      "}\n"
                      "popover button:hover { background: " +
                      surface1 +
                      "; }\n"
                      "popover button.suggested-action {"
                      "  background: " +
                      toHex(colors.blue) +
                      ";"
                      "  color: " +
                      base +
                      ";"
                      "}\n"
                      "popover button.suggested-action:hover {"
                      "  background: " +
                      toHex(colors.sky) +
                      ";"
                      "  color: " +
                      base +
                      ";"
                      "}\n"
                      "popover separator { background: " +
                      overlay0 +
                      "; }\n"

                      /* dropdown popup list */
                      "dropdown popover > contents { background: " +
                      surface0 +
                      "; }\n"
                      "dropdown popover > arrow { background: " +
                      surface0 +
                      "; }\n"
                      "dropdown popover listview { background: transparent; }\n"
                      "dropdown popover listview row {"
                      "  color: " +
                      text +
                      ";"
                      "  background: transparent;"
                      "}\n"
                      "dropdown popover listview row:hover {"
                      "  background: " +
                      surface1 +
                      ";"
                      "}\n"
                      "dropdown popover listview row:selected {"
                      "  background: " +
                      surface1 +
                      ";"
                      "  color: " +
                      text +
                      ";"
                      "}\n";

    gtk_css_provider_load_from_string(themeProvider, css.c_str());
}

void LinuxPlatform::updateLicenseButton() {
    // License button text is always "Manage License..."
}

void LinuxPlatform::onThemeLightClicked(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    if (platform->app_) {
        platform->app_->setDarkTheme(false);
    }
    platform->updateThemeButtons();
    platform->updateGtkTheme();
}

void LinuxPlatform::onThemeDarkClicked(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    if (platform->app_) {
        platform->app_->setDarkTheme(true);
    }
    platform->updateThemeButtons();
    platform->updateGtkTheme();
}

void LinuxPlatform::onThemeAutoClicked(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    if (platform->app_) {
        // Detect system theme preference via GTK settings
        GtkSettings* settings = gtk_settings_get_default();
        gchar* themeName = nullptr;
        g_object_get(settings, "gtk-theme-name", &themeName, nullptr);
        bool systemIsDark = themeName && g_str_has_suffix(themeName, "-dark");
        g_free(themeName);
        platform->app_->setDarkTheme(systemIsDark);
    }
    platform->updateThemeButtons();
    platform->updateGtkTheme();
}

void LinuxPlatform::onLicenseClicked(GtkButton* button, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    gtk_popover_popdown(GTK_POPOVER(platform->menuPopover_));
    platform->showLicenseDialog();
}

void LinuxPlatform::showLicenseDialog() {
    auto& licenseManager = LicenseManager::instance();

    // Create the dialog window
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Manage License");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget* mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(mainBox, 24);
    gtk_widget_set_margin_end(mainBox, 24);
    gtk_widget_set_margin_top(mainBox, 24);
    gtk_widget_set_margin_bottom(mainBox, 24);

    // Status label for messages
    GtkWidget* statusLabel = gtk_label_new("");
    gtk_widget_set_halign(statusLabel, GTK_ALIGN_START);

    if (licenseManager.hasValidLicense()) {
        // Licensed view
        const auto& info = licenseManager.getLicenseInfo();

        std::string maskedKey = info.licenseKey;
        if (maskedKey.length() > 8) {
            maskedKey = maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
        }

        // Status indicator
        GtkWidget* statusBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* statusIcon = gtk_image_new_from_icon_name("emblem-ok-symbolic");
        gtk_widget_add_css_class(statusIcon, "success");
        GtkWidget* statusText = gtk_label_new("License Active");
        gtk_widget_add_css_class(statusText, "title-3");
        gtk_box_append(GTK_BOX(statusBox), statusIcon);
        gtk_box_append(GTK_BOX(statusBox), statusText);
        gtk_box_append(GTK_BOX(mainBox), statusBox);

        // License info
        GtkWidget* infoGrid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(infoGrid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(infoGrid), 12);

        GtkWidget* emailLabel = gtk_label_new("Email:");
        gtk_widget_add_css_class(emailLabel, "dim-label");
        gtk_widget_set_halign(emailLabel, GTK_ALIGN_END);
        GtkWidget* emailValue =
            gtk_label_new(info.customerEmail.empty() ? "N/A" : info.customerEmail.c_str());
        gtk_widget_set_halign(emailValue, GTK_ALIGN_START);
        gtk_label_set_selectable(GTK_LABEL(emailValue), TRUE);
        gtk_grid_attach(GTK_GRID(infoGrid), emailLabel, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(infoGrid), emailValue, 1, 0, 1, 1);

        GtkWidget* keyLabel = gtk_label_new("Key:");
        gtk_widget_add_css_class(keyLabel, "dim-label");
        gtk_widget_set_halign(keyLabel, GTK_ALIGN_END);
        GtkWidget* keyValue = gtk_label_new(maskedKey.c_str());
        gtk_widget_set_halign(keyValue, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(infoGrid), keyLabel, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(infoGrid), keyValue, 1, 1, 1, 1);

        gtk_box_append(GTK_BOX(mainBox), infoGrid);

        // Status message
        gtk_box_append(GTK_BOX(mainBox), statusLabel);

        // Buttons
        GtkWidget* buttonBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttonBox, GTK_ALIGN_END);
        gtk_widget_set_margin_top(buttonBox, 8);

        GtkWidget* deactivateButton = gtk_button_new_with_label("Deactivate");
        gtk_widget_add_css_class(deactivateButton, "destructive-action");
        GtkWidget* closeButton = gtk_button_new_with_label("Close");

        gtk_box_append(GTK_BOX(buttonBox), deactivateButton);
        gtk_box_append(GTK_BOX(buttonBox), closeButton);
        gtk_box_append(GTK_BOX(mainBox), buttonBox);

        g_object_set_data(G_OBJECT(dialog), "status", statusLabel);
        g_object_set_data(G_OBJECT(dialog), "deactivate_btn", deactivateButton);

        g_signal_connect(closeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer dlg) {
                             gtk_window_destroy(GTK_WINDOW(dlg));
                         }),
                         dialog);

        g_signal_connect(
            deactivateButton, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer dlg) {
                GtkWidget* status = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "status"));
                gtk_label_set_text(GTK_LABEL(status), "Deactivating...");
                gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);

                GtkWidget* dialogRef = GTK_WIDGET(dlg);
                LicenseManager::instance().deactivateLicense([dialogRef](
                                                                 const LicenseInfo& result) {
                    g_idle_add(
                        +[](gpointer data) -> gboolean {
                            auto* info = static_cast<std::pair<GtkWidget*, LicenseInfo>*>(data);
                            if (GTK_IS_WINDOW(info->first)) {
                                if (info->second.error.empty()) {
                                    gtk_window_destroy(GTK_WINDOW(info->first));
                                } else {
                                    GtkWidget* st = GTK_WIDGET(
                                        g_object_get_data(G_OBJECT(info->first), "status"));
                                    GtkWidget* btn = GTK_WIDGET(
                                        g_object_get_data(G_OBJECT(info->first), "deactivate_btn"));
                                    gtk_label_set_text(GTK_LABEL(st), info->second.error.c_str());
                                    gtk_widget_set_sensitive(btn, TRUE);
                                }
                            }
                            delete info;
                            return G_SOURCE_REMOVE;
                        },
                        new std::pair<GtkWidget*, LicenseInfo>(dialogRef, result));
                });
            }),
            dialog);

    } else {
        // Unlicensed view
        GtkWidget* titleLabel = gtk_label_new("Register License");
        gtk_widget_add_css_class(titleLabel, "title-3");
        gtk_widget_set_halign(titleLabel, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(mainBox), titleLabel);

        GtkWidget* descLabel = gtk_label_new("Enter your license key to activate DearSQL:");
        gtk_widget_set_halign(descLabel, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(mainBox), descLabel);

        GtkWidget* entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "XXXX-XXXX-XXXX-XXXX");
        gtk_box_append(GTK_BOX(mainBox), entry);

        // Status message
        gtk_box_append(GTK_BOX(mainBox), statusLabel);

        GtkWidget* linkBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(linkBox, GTK_ALIGN_START);
        GtkWidget* linkText = gtk_label_new("Don't have a license?");
        gtk_widget_add_css_class(linkText, "dim-label");
        GtkWidget* linkButton = gtk_link_button_new_with_label(
            "https://dearsql.lemonsqueezy.com/checkout/buy/8d4644a9-dfcb-4a06-aeab-a8890d082673",
            "Purchase one");
        gtk_box_append(GTK_BOX(linkBox), linkText);
        gtk_box_append(GTK_BOX(linkBox), linkButton);
        gtk_box_append(GTK_BOX(mainBox), linkBox);

        // Buttons
        GtkWidget* buttonBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(buttonBox, GTK_ALIGN_END);
        gtk_widget_set_margin_top(buttonBox, 8);

        GtkWidget* cancelButton = gtk_button_new_with_label("Cancel");
        GtkWidget* activateButton = gtk_button_new_with_label("Activate");
        gtk_widget_add_css_class(activateButton, "suggested-action");

        gtk_box_append(GTK_BOX(buttonBox), cancelButton);
        gtk_box_append(GTK_BOX(buttonBox), activateButton);
        gtk_box_append(GTK_BOX(mainBox), buttonBox);

        g_object_set_data(G_OBJECT(dialog), "entry", entry);
        g_object_set_data(G_OBJECT(dialog), "status", statusLabel);
        g_object_set_data(G_OBJECT(dialog), "activate_btn", activateButton);

        g_signal_connect(cancelButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer dlg) {
                             gtk_window_destroy(GTK_WINDOW(dlg));
                         }),
                         dialog);

        g_signal_connect(
            activateButton, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer dlg) {
                GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "entry"));
                GtkWidget* status = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "status"));

                const char* key = gtk_editable_get_text(GTK_EDITABLE(entry));
                if (!key || strlen(key) == 0) {
                    gtk_label_set_text(GTK_LABEL(status), "Please enter a license key");
                    return;
                }

                gtk_label_set_text(GTK_LABEL(status), "Activating...");
                gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);

                std::string licenseKey = key;
                GtkWidget* dialogRef = GTK_WIDGET(dlg);

                LicenseManager::instance().activateLicense(
                    licenseKey, [dialogRef](const LicenseInfo& result) {
                        g_idle_add(
                            +[](gpointer data) -> gboolean {
                                auto* info = static_cast<std::pair<GtkWidget*, LicenseInfo>*>(data);
                                if (GTK_IS_WINDOW(info->first)) {
                                    if (info->second.valid) {
                                        gtk_window_destroy(GTK_WINDOW(info->first));
                                    } else {
                                        GtkWidget* st = GTK_WIDGET(
                                            g_object_get_data(G_OBJECT(info->first), "status"));
                                        GtkWidget* btn = GTK_WIDGET(g_object_get_data(
                                            G_OBJECT(info->first), "activate_btn"));
                                        std::string err = info->second.error.empty()
                                                              ? "Activation failed"
                                                              : info->second.error;
                                        gtk_label_set_text(GTK_LABEL(st), err.c_str());
                                        gtk_widget_set_sensitive(btn, TRUE);
                                    }
                                }
                                delete info;
                                return G_SOURCE_REMOVE;
                            },
                            new std::pair<GtkWidget*, LicenseInfo>(dialogRef, result));
                    });
            }),
            dialog);
    }

    gtk_window_set_child(GTK_WINDOW(dialog), mainBox);
    gtk_window_present(GTK_WINDOW(dialog));
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
        if (app_ && app_->isShutdownRequested()) {
            shouldClose_ = true;
        }

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
