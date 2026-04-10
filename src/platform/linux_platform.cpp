#if defined(__linux__)

#include "platform/linux_platform.hpp"
#include "application.hpp"
#include "config.hpp"
#include "database/async_helper.hpp"
#include "platform/updater.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/x11/gdkx.h>
#endif

namespace {
    constexpr double kIdleActivationDelaySeconds = 2.0;

    GdkModifierType normalizeModifierStateForKeyEvent(GdkModifierType state, guint keyval,
                                                      bool pressed) {
        auto setOrClearMask = [&](GdkModifierType mask) {
            if (pressed)
                state = static_cast<GdkModifierType>(state | mask);
            else
                state = static_cast<GdkModifierType>(state & ~mask);
        };

        switch (keyval) {
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R:
            setOrClearMask(GDK_SHIFT_MASK);
            break;
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R:
            setOrClearMask(GDK_CONTROL_MASK);
            break;
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
            setOrClearMask(GDK_ALT_MASK);
            break;
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R:
            setOrClearMask(GDK_SUPER_MASK);
            break;
        default:
            break;
        }

        return state;
    }

} // namespace

LinuxPlatform::LinuxPlatform(Application* app)
    : app_(app), window_(nullptr), shouldClose_(false), mouseX_(0), mouseY_(0),
      lastInteractionTimeUs_(g_get_monotonic_time()) {}

LinuxPlatform::~LinuxPlatform() {
    cleanup();
}

bool LinuxPlatform::initializeGTK(int*, char***) {
    if (!gtk_init_check()) {
        std::cerr << "Failed to initialize GTK" << std::endl;
        return false;
    }

    g_set_prgname(APP_IDENTIFIER);
    g_set_application_name(APP_NAME);

    window_ = gtk_window_new();
    gtk_widget_add_css_class(window_, "dearsql-main");
    gtk_window_set_title(GTK_WINDOW(window_), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 720);

    g_signal_connect(window_, "close-request", G_CALLBACK(onClose), this);

    // create OpenGL backend (owns the GtkGLArea widget)
    backend_ = std::make_unique<LinuxOpenGLBackend>();
    GtkWidget* glArea = backend_->getGLArea();

    // connect GL area signals
    g_signal_connect(glArea, "realize", G_CALLBACK(onRealize), this);
    g_signal_connect(glArea, "render", G_CALLBACK(onRender), this);
    g_signal_connect(glArea, "resize", G_CALLBACK(onResize), this);

    // setup input handlers (uses backend_->getGLArea())
    setupInputHandlers();

    gtk_window_set_child(GTK_WINDOW(window_), glArea);

    std::cout << "GTK window created successfully" << std::endl;
    return true;
}

bool LinuxPlatform::initializePlatform(GLFWwindow*) {
    return true;
}

bool LinuxPlatform::initializeImGuiBackend() {
    // backend initializes inside onRealize once the GL context is ready
    return backend_ && backend_->getGLArea() != nullptr;
}

void LinuxPlatform::setupTitlebar() {
    titlebar_ = std::make_unique<LinuxTitlebar>(app_, window_);
    titlebar_->setInteractionCallback([this] { noteInteraction(); });
    titlebar_->setup();
}

void LinuxPlatform::updateWorkspaceDropdown() {
    if (titlebar_)
        titlebar_->updateWorkspaceDropdown();
}

void LinuxPlatform::applyCurrentTheme() {
    if (titlebar_)
        titlebar_->applyTheme(app_ ? app_->isDarkTheme() : false);
}

float LinuxPlatform::getTitlebarHeight() const {
    return titlebar_ ? titlebar_->getHeight() : 0.0f;
}

void LinuxPlatform::onSidebarToggleClicked() {
    if (app_) {
        app_->setSidebarVisible(!app_->isSidebarVisible());
    }
}

void LinuxPlatform::cleanup() {
    titlebar_.reset();
    backend_.reset();

    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void LinuxPlatform::renderFrame() {
    if (backend_ && backend_->getGLArea()) {
        gtk_gl_area_queue_render(GTK_GL_AREA(backend_->getGLArea()));
    }

    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

void LinuxPlatform::shutdownImGui() {
    if (backend_)
        backend_->shutdown();
}

bool LinuxPlatform::shouldClose() const {
    return shouldClose_;
}

void LinuxPlatform::getFramebufferSize(int* width, int* height) const {
    if (backend_) {
        backend_->getFramebufferSize(*width, *height);
    } else {
        *width = 1280;
        *height = 720;
    }
}

void LinuxPlatform::pollEvents() {
    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, FALSE);
    }
}

ImTextureID LinuxPlatform::createTextureFromRGBA(const uint8_t* pixels, int width, int height) {
    return backend_ ? backend_->createTextureFromRGBA(pixels, width, height) : ImTextureID{};
}

// ---- static GL area callbacks ----

void LinuxPlatform::onRealize(GtkGLArea* area, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    gtk_gl_area_make_current(area);

    if (gtk_gl_area_get_error(area) != nullptr) {
        std::cerr << "Failed to initialize OpenGL context" << std::endl;
        return;
    }

    if (platform->backend_) {
        platform->backend_->initializeImGui();
    }
}

gboolean LinuxPlatform::onRender(GtkGLArea*, GdkGLContext*, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);

    if (!platform->backend_)
        return FALSE;

    bool darkTheme = platform->app_->isDarkTheme();
    ImVec4 clearColor =
        darkTheme ? ImVec4(0.110f, 0.110f, 0.137f, 1.0f) : ImVec4(0.957f, 0.957f, 0.957f, 1.0f);

    platform->backend_->beginFrame(clearColor);

    ImGuiIO& io = ImGui::GetIO();
    int fbW = 1280, fbH = 720;
    platform->backend_->getFramebufferSize(fbW, fbH);
    io.DisplaySize = ImVec2(static_cast<float>(fbW), static_cast<float>(fbH));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    io.AddMousePosEvent(static_cast<float>(platform->mouseX_),
                        static_cast<float>(platform->mouseY_));

    ImGui::NewFrame();

    platform->app_->renderMainUI();

    ImGui::Render();

    // update GTK cursor only when ImGui's requested cursor changes —
    // gtk_widget_set_cursor_from_name loads an XCursor image each call and
    // was the top allocation hot spot in heaptrack profiling.
    ImGuiMouseCursor imguiCursor = ImGui::GetMouseCursor();
    if (imguiCursor != platform->lastCursor_) {
        const char* cursorName = nullptr;
        switch (imguiCursor) {
        case ImGuiMouseCursor_Arrow:
            cursorName = "default";
            break;
        case ImGuiMouseCursor_TextInput:
            cursorName = "text";
            break;
        case ImGuiMouseCursor_ResizeNS:
            cursorName = "ns-resize";
            break;
        case ImGuiMouseCursor_ResizeEW:
            cursorName = "ew-resize";
            break;
        case ImGuiMouseCursor_ResizeNESW:
            cursorName = "nesw-resize";
            break;
        case ImGuiMouseCursor_ResizeNWSE:
            cursorName = "nwse-resize";
            break;
        case ImGuiMouseCursor_Hand:
            cursorName = "pointer";
            break;
        case ImGuiMouseCursor_NotAllowed:
            cursorName = "not-allowed";
            break;
        default:
            cursorName = "default";
            break;
        }
        gtk_widget_set_cursor_from_name(platform->backend_->getGLArea(), cursorName);
        platform->lastCursor_ = imguiCursor;
    }

    platform->backend_->renderDrawData(ImGui::GetDrawData());

    return TRUE;
}

void LinuxPlatform::onResize(GtkGLArea* area, gint width, gint height, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    if (platform->backend_) {
        platform->backend_->onResize(width, height);
        // immediately redraw at the new size so there's no blank frame during resize
        gtk_gl_area_queue_render(area);
    }
}

// ---- input callbacks ----

gboolean LinuxPlatform::onDrop(GtkDropTarget*, const GValue* value, double, double,
                               gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    if (!platform || !platform->app_ || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;

    platform->noteInteraction();

    auto* files = static_cast<GSList*>(g_value_get_boxed(value));
    for (GSList* l = files; l; l = l->next) {
        char* path = g_file_get_path(G_FILE(l->data));
        if (path) {
            platform->app_->openFile(std::string(path));
            g_free(path);
        }
    }
    if (platform->window_) {
        gtk_window_present(GTK_WINDOW(platform->window_));
    }
    return TRUE;
}

void LinuxPlatform::setupInputHandlers() {
    GtkWidget* glArea = backend_->getGLArea();

    GtkEventController* keyController = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keyController, GTK_PHASE_CAPTURE);
    g_signal_connect(keyController, "key-pressed", G_CALLBACK(onKeyPress), this);
    g_signal_connect(keyController, "key-released", G_CALLBACK(onKeyRelease), this);
    gtk_widget_add_controller(glArea, keyController);

    GtkEventController* motionController = gtk_event_controller_motion_new();
    g_signal_connect(motionController, "motion", G_CALLBACK(onMotionNotify), this);
    gtk_widget_add_controller(glArea, motionController);

    GtkGesture* clickGesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(clickGesture), 0);
    g_signal_connect(clickGesture, "pressed", G_CALLBACK(onButtonPress), this);
    g_signal_connect(clickGesture, "released", G_CALLBACK(onButtonRelease), this);
    gtk_widget_add_controller(glArea, GTK_EVENT_CONTROLLER(clickGesture));

    GtkEventController* scrollController =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scrollController, "scroll", G_CALLBACK(onScroll), this);
    gtk_widget_add_controller(glArea, scrollController);

    GtkDropTarget* dropTarget = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(dropTarget, "drop", G_CALLBACK(LinuxPlatform::onDrop), this);
    gtk_widget_add_controller(glArea, GTK_EVENT_CONTROLLER(dropTarget));
}

gboolean LinuxPlatform::onKeyPress(GtkEventControllerKey*, guint keyval, guint,
                                   GdkModifierType state, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    platform->noteInteraction();
    platform->updateImGuiKeyMods(normalizeModifierStateForKeyEvent(state, keyval, true));

    ImGuiKey key = platform->gtkKeyToImGuiKey(keyval);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, true);
    }

    if (!(state & GDK_CONTROL_MASK)) {
        gunichar unicode = gdk_keyval_to_unicode(keyval);
        if (unicode != 0 && unicode < 0x10000) {
            io.AddInputCharacter(unicode);
        }
    }

    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        return TRUE;
    }

    return io.WantCaptureKeyboard ? TRUE : FALSE;
}

gboolean LinuxPlatform::onKeyRelease(GtkEventControllerKey*, guint keyval, guint,
                                     GdkModifierType state, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    platform->noteInteraction();
    platform->updateImGuiKeyMods(normalizeModifierStateForKeyEvent(state, keyval, false));

    ImGuiKey key = platform->gtkKeyToImGuiKey(keyval);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, false);
    }

    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        return TRUE;
    }

    return io.WantCaptureKeyboard ? TRUE : FALSE;
}

void LinuxPlatform::onMotionNotify(GtkEventControllerMotion*, gdouble x, gdouble y,
                                   gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->noteInteraction();
    platform->mouseX_ = x;
    platform->mouseY_ = y;

    if (platform->backend_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->backend_->getGLArea()));
    }
}

void LinuxPlatform::onButtonPress(GtkGestureClick* gesture, gint, gdouble, gdouble,
                                  gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    platform->noteInteraction();
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    int imguiButton = 0;
    if (button == GDK_BUTTON_PRIMARY)
        imguiButton = 0;
    else if (button == GDK_BUTTON_SECONDARY)
        imguiButton = 1;
    else if (button == GDK_BUTTON_MIDDLE)
        imguiButton = 2;

    io.AddMouseButtonEvent(imguiButton, true);

    if (platform->backend_) {
        gtk_widget_grab_focus(platform->backend_->getGLArea());
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->backend_->getGLArea()));
    }
}

void LinuxPlatform::onButtonRelease(GtkGestureClick* gesture, gint, gdouble, gdouble,
                                    gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();

    platform->noteInteraction();
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

    int imguiButton = 0;
    if (button == GDK_BUTTON_PRIMARY)
        imguiButton = 0;
    else if (button == GDK_BUTTON_SECONDARY)
        imguiButton = 1;
    else if (button == GDK_BUTTON_MIDDLE)
        imguiButton = 2;

    io.AddMouseButtonEvent(imguiButton, false);

    if (platform->backend_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->backend_->getGLArea()));
    }
}

gboolean LinuxPlatform::onScroll(GtkEventControllerScroll* controller, gdouble dx, gdouble dy,
                                 gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    ImGuiIO& io = ImGui::GetIO();
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));

    platform->noteInteraction();
    float wheelX = static_cast<float>(-dx);
    float wheelY = static_cast<float>(-dy);

    bool shiftHeld = (state & GDK_SHIFT_MASK) != 0 || ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                     ImGui::IsKeyDown(ImGuiKey_RightShift);
    if (shiftHeld && std::fabs(wheelY) < 1e-6f && std::fabs(wheelX) > 1e-6f) {
        wheelY = wheelX;
        wheelX = 0.0f;
    }

    if (std::fabs(wheelX) > 1e-6f || std::fabs(wheelY) > 1e-6f) {
        io.AddMouseWheelEvent(wheelX, wheelY);
    }

    if (platform->backend_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(platform->backend_->getGLArea()));
    }

    return TRUE;
}

gboolean LinuxPlatform::onClose(GtkWindow*, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    platform->shouldClose_ = true;
    return TRUE;
}

void LinuxPlatform::noteInteraction() {
    lastInteractionTimeUs_ = g_get_monotonic_time();
    ensureTickCallback();
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
    case GDK_KEY_ISO_Left_Tab:
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

void LinuxPlatform::ensureTickCallback() {
    if (tickCallbackId_ == 0 && backend_ && backend_->getGLArea()) {
        tickCallbackId_ =
            gtk_widget_add_tick_callback(backend_->getGLArea(), onTickCallback, this, nullptr);
    }
}

gboolean LinuxPlatform::onTickCallback(GtkWidget* widget, GdkFrameClock*, gpointer userData) {
    auto* platform = static_cast<LinuxPlatform*>(userData);
    if (platform->shouldClose_) {
        platform->tickCallbackId_ = 0;
        return G_SOURCE_REMOVE;
    }

    // show/hide update button based on background version check
    if (platform->titlebar_) {
        GtkWidget* updateButton = platform->titlebar_->getUpdateButton();
        if (updateButton) {
            bool available = isUpdateAvailable();
            if (available != static_cast<bool>(gtk_widget_get_visible(updateButton))) {
                gtk_widget_set_visible(updateButton, available);
                if (available) {
                    auto version = getLatestVersion();
                    auto tooltip = "Update available: v" + version;
                    gtk_widget_set_tooltip_text(updateButton, tooltip.c_str());
                }
            }
        }
    }

    const bool windowFocused = gtk_window_is_active(GTK_WINDOW(platform->window_));
    if (windowFocused && !platform->lastWindowFocused_) {
        platform->noteInteraction();
    }
    platform->lastWindowFocused_ = windowFocused;

    const bool hasAsyncWork = AsyncOperationControl::hasRunningTasks();
    const bool asyncJustFinished = platform->lastHadAsyncWork_ && !hasAsyncWork;
    platform->lastHadAsyncWork_ = hasAsyncWork;
    const double timeSinceInteraction =
        static_cast<double>(
            std::max<gint64>(0, g_get_monotonic_time() - platform->lastInteractionTimeUs_)) /
        1000000.0;
    const bool idleBecauseUnfocused = !windowFocused && !hasAsyncWork && !asyncJustFinished;
    const bool idleBecauseInactive = windowFocused &&
                                     (timeSinceInteraction >= kIdleActivationDelaySeconds) &&
                                     !hasAsyncWork && !asyncJustFinished;

    if (idleBecauseUnfocused || idleBecauseInactive) {
        // stop the frame clock to save power; noteInteraction() re-enables it
        platform->tickCallbackId_ = 0;
        return G_SOURCE_REMOVE;
    }

    if (hasAsyncWork || asyncJustFinished || windowFocused) {
        gtk_gl_area_queue_render(GTK_GL_AREA(widget));
    }

    return G_SOURCE_CONTINUE;
}

void LinuxPlatform::runMainLoop() {
    gtk_window_present(GTK_WINDOW(window_));
    noteInteraction();
    lastWindowFocused_ = gtk_window_is_active(GTK_WINDOW(window_));

    // tick callback drives rendering in sync with the GDK frame clock
    ensureTickCallback();

    // low-frequency timer wakes the main loop when idle to check for async work
    auto* self = this;
    guint idleTimerId = g_timeout_add(
        200,
        +[](gpointer data) -> gboolean {
            auto* platform = static_cast<LinuxPlatform*>(data);
            if (platform->tickCallbackId_ == 0 && AsyncOperationControl::hasRunningTasks()) {
                platform->ensureTickCallback();
            }
            return G_SOURCE_CONTINUE;
        },
        self);

    while (!shouldClose_) {
        if (app_ && app_->isShutdownRequested()) {
            shouldClose_ = true;
            break;
        }
        g_main_context_iteration(nullptr, TRUE);
    }

    g_source_remove(idleTimerId);

    if (tickCallbackId_) {
        gtk_widget_remove_tick_callback(backend_->getGLArea(), tickCallbackId_);
        tickCallbackId_ = 0;
    }
}

#endif // defined(__linux__)
