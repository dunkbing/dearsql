#if defined(__linux__)
// Must include epoxy BEFORE any other GL headers
#include <epoxy/gl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "platform/graphics_backend.hpp"
#include "platform/opengl_texture.hpp"
#include <gtk/gtk.h>
#include <iostream>

// GTK4 clipboard globals
static GdkClipboard* g_GtkClipboard = nullptr;
static char* g_ClipboardText = nullptr;
static bool g_ClipboardDirty = true;
static bool g_ClipboardReadPending = false;

static void clipboard_read_callback(GObject* source, GAsyncResult* result, gpointer) {
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

static void clipboard_changed_callback(GdkClipboard* clipboard, gpointer) {
    g_ClipboardDirty = true;

    if (!g_ClipboardReadPending) {
        g_ClipboardReadPending = true;
        gdk_clipboard_read_text_async(clipboard, nullptr, clipboard_read_callback, nullptr);
    }
}

static const char* ImGui_ImplGtk_GetClipboardText(void*) {
    if (!g_GtkClipboard) {
        return "";
    }

    if (g_ClipboardDirty && !g_ClipboardReadPending) {
        g_ClipboardReadPending = true;
        gdk_clipboard_read_text_async(g_GtkClipboard, nullptr, clipboard_read_callback, nullptr);
    }

    if (g_ClipboardReadPending) {
        GMainContext* context = g_main_context_default();
        gint64 end_time = g_get_monotonic_time() + 200000; // 200ms timeout

        while (g_ClipboardReadPending && g_get_monotonic_time() < end_time) {
            g_main_context_iteration(context, TRUE);
        }
    }

    return g_ClipboardText ? g_ClipboardText : "";
}

static void ImGui_ImplGtk_SetClipboardText(void*, const char* text) {
    if (g_GtkClipboard && text) {
        gdk_clipboard_set_text(g_GtkClipboard, text);
    }
}

void cleanupLinuxClipboard() {
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
}

LinuxOpenGLBackend::LinuxOpenGLBackend() {
    glArea_ = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(glArea_), 3, 3);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(glArea_), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(glArea_), FALSE);
    // disable auto-render so only explicit gtk_gl_area_queue_render() calls trigger frames
    gtk_gl_area_set_auto_render(GTK_GL_AREA(glArea_), FALSE);
    gtk_widget_set_hexpand(glArea_, TRUE);
    gtk_widget_set_vexpand(glArea_, TRUE);
    gtk_widget_set_focusable(glArea_, TRUE);
    gtk_widget_set_can_focus(glArea_, TRUE);
}

bool LinuxOpenGLBackend::initializeImGui() {
    ImGui_ImplOpenGL3_Init("#version 330");

    // setup clipboard
    GdkDisplay* display = gtk_widget_get_display(glArea_);
    g_GtkClipboard = gdk_display_get_clipboard(display);

    g_signal_connect(g_GtkClipboard, "changed", G_CALLBACK(clipboard_changed_callback), nullptr);

    // pre-fetch initial clipboard content
    g_ClipboardDirty = true;
    g_ClipboardReadPending = true;
    gdk_clipboard_read_text_async(g_GtkClipboard, nullptr, clipboard_read_callback, nullptr);

    ImGuiIO& io = ImGui::GetIO();
    io.GetClipboardTextFn = ImGui_ImplGtk_GetClipboardText;
    io.SetClipboardTextFn = ImGui_ImplGtk_SetClipboardText;

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    return true;
}

void LinuxOpenGLBackend::beginFrame(ImVec4 clearColor) {
    glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_NewFrame();
}

void LinuxOpenGLBackend::renderDrawData(ImDrawData* drawData) {
    glViewport(0, 0, fbWidth_, fbHeight_);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

void LinuxOpenGLBackend::onResize(int width, int height) {
    fbWidth_ = width;
    fbHeight_ = height;
}

void LinuxOpenGLBackend::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    cleanupLinuxClipboard();
    std::cout << "ImGui OpenGL backend shutdown" << std::endl;
}

void LinuxOpenGLBackend::getFramebufferSize(int& width, int& height) {
    width = fbWidth_;
    height = fbHeight_;
}

ImTextureID LinuxOpenGLBackend::createTextureFromRGBA(const uint8_t* pixels, int width,
                                                      int height) {
    return createOpenGLTextureFromRGBA(pixels, width, height);
}

#endif // defined(__linux__)
