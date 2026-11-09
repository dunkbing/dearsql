#if defined(__linux__)

#include "application.hpp"
#include "platform/alert.hpp"
#include <gtk/gtk.h>
#include <vector>

struct AlertCallbackData {
    std::vector<AlertButton> buttons;
};

struct DeferredAlertData {
    std::string title;
    std::string message;
    std::vector<AlertButton> buttons;
};

#if !GTK_CHECK_VERSION(4, 10, 0)
// gtk < 4.10 (ubuntu 22.04) has no GtkAlertDialog
static void onMessageDialogResponse(GtkDialog* dialog, int response, gpointer userData) {
    auto* data = static_cast<AlertCallbackData*>(userData);
    if (response >= 0 && response < static_cast<int>(data->buttons.size()) &&
        data->buttons[response].onPress) {
        data->buttons[response].onPress();
    }
    delete data;
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static gboolean showAlertIdle(gpointer userData) {
    auto* deferred = static_cast<DeferredAlertData*>(userData);

    GtkWindow* parent = nullptr;
    if (void* host = Application::getInstance().nativeWindow())
        parent = GTK_WINDOW(host);

    GtkWidget* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_OTHER,
                                               GTK_BUTTONS_NONE, "%s", deferred->title.c_str());
    if (!deferred->message.empty()) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                                 deferred->message.c_str());
    }
    for (int i = 0; i < static_cast<int>(deferred->buttons.size()); ++i) {
        gtk_dialog_add_button(GTK_DIALOG(dialog), deferred->buttons[i].text.c_str(), i);
        if (deferred->buttons[i].style == AlertButton::Style::Default) {
            gtk_dialog_set_default_response(GTK_DIALOG(dialog), i);
        }
    }

    auto* cbData = new AlertCallbackData{std::move(deferred->buttons)};
    g_signal_connect(dialog, "response", G_CALLBACK(onMessageDialogResponse), cbData);
    gtk_window_present(GTK_WINDOW(dialog));

    delete deferred;
    return G_SOURCE_REMOVE;
}
#else
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
    if (void* host = Application::getInstance().nativeWindow())
        parent = GTK_WINDOW(host);

    auto* cbData = new AlertCallbackData{std::move(deferred->buttons)};
    gtk_alert_dialog_choose(dialog, parent, nullptr, onAlertResponse, cbData);
    g_object_unref(dialog);

    delete deferred;
    return G_SOURCE_REMOVE;
}

#endif

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
