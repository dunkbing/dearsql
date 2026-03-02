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

void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty()) {
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    }

    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", title.c_str());

    if (!message.empty()) {
        gtk_alert_dialog_set_detail(dialog, message.c_str());
    }

    // Build button labels
    std::vector<const char*> labels;
    labels.reserve(buttons.size());
    // Keep c_str() pointers valid by referencing buttons directly
    for (const auto& btn : buttons) {
        labels.push_back(btn.text.c_str());
    }
    labels.push_back(nullptr); // null-terminated
    gtk_alert_dialog_set_buttons(dialog, labels.data());

    // Set cancel and default button indices
    int cancelIndex = -1;
    int defaultIndex = -1;
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        if (buttons[i].style == AlertButton::Style::Cancel && cancelIndex == -1) {
            cancelIndex = i;
        }
        if (buttons[i].style == AlertButton::Style::Default) {
            defaultIndex = i;
        }
    }
    if (cancelIndex >= 0) {
        gtk_alert_dialog_set_cancel_button(dialog, cancelIndex);
    }
    if (defaultIndex >= 0) {
        gtk_alert_dialog_set_default_button(dialog, defaultIndex);
    }

    // Get parent GTK window
    GtkWindow* parent = nullptr;
    auto* platform = dynamic_cast<LinuxPlatform*>(Application::getInstance().getPlatform());
    if (platform) {
        parent = GTK_WINDOW(platform->getGtkWindow());
    }

    auto* data = new AlertCallbackData{std::move(buttons)};
    gtk_alert_dialog_choose(dialog, parent, nullptr, onAlertResponse, data);
    g_object_unref(dialog);
}

#endif
