#if defined(__linux__)

#include "application.hpp"
#include "platform/alert.hpp"
#include "platform/linux_platform.hpp"
#include <gtk/gtk.h>
#include <vector>

struct AlertCallbackData {
    std::vector<AlertButton> buttons;
};

static void onAlertResponse(GObject* source, GAsyncResult* result, gpointer userData) {
    auto* data = static_cast<AlertCallbackData*>(userData);
    auto* dialog = GTK_ALERT_DIALOG(source);

    GError* error = nullptr;
    int chosen = gtk_alert_dialog_choose_finish(dialog, result, &error);

    if (error) {
        // User dismissed the dialog (e.g. closed the window)
        g_error_free(error);
        delete data;
        return;
    }

    if (chosen >= 0 && chosen < static_cast<int>(data->buttons.size())) {
        if (data->buttons[chosen].onPress) {
            data->buttons[chosen].onPress();
        }
    }

    delete data;
}

struct DeferredAlertData {
    std::string title;
    std::string message;
    std::vector<AlertButton> buttons;
};

static gboolean showAlertIdle(gpointer userData) {
    auto* deferred = static_cast<DeferredAlertData*>(userData);

    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", deferred->title.c_str());

    if (!deferred->message.empty()) {
        gtk_alert_dialog_set_detail(dialog, deferred->message.c_str());
    }

    // Build button labels
    std::vector<const char*> labels;
    labels.reserve(deferred->buttons.size());
    for (const auto& btn : deferred->buttons) {
        labels.push_back(btn.text.c_str());
    }
    labels.push_back(nullptr);
    gtk_alert_dialog_set_buttons(dialog, labels.data());

    // Set cancel and default button indices
    int cancelIndex = -1;
    int defaultIndex = -1;
    for (int i = 0; i < static_cast<int>(deferred->buttons.size()); ++i) {
        if (deferred->buttons[i].style == AlertButton::Style::Cancel && cancelIndex == -1) {
            cancelIndex = i;
        }
        if (deferred->buttons[i].style == AlertButton::Style::Default) {
            defaultIndex = i;
        }
    }
    if (cancelIndex >= 0) {
        gtk_alert_dialog_set_cancel_button(dialog, cancelIndex);
    }
    if (defaultIndex >= 0) {
        gtk_alert_dialog_set_default_button(dialog, defaultIndex);
    }

    GtkWindow* parent = nullptr;
    auto* platform = dynamic_cast<LinuxPlatform*>(Application::getInstance().getPlatform());
    if (platform) {
        parent = GTK_WINDOW(platform->getGtkWindow());
    }

    auto* cbData = new AlertCallbackData{std::move(deferred->buttons)};
    gtk_alert_dialog_choose(dialog, parent, nullptr, onAlertResponse, cbData);
    g_object_unref(dialog);

    delete deferred;
    return G_SOURCE_REMOVE;
}

void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty()) {
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    }

    // defer to next main loop iteration so the ImGui popup closes cleanly first
    auto* deferred = new DeferredAlertData{title, message, std::move(buttons)};
    g_idle_add(showAlertIdle, deferred);
}

#endif
