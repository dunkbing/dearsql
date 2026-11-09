#if defined(__linux__)

#include "application.hpp"
#include "ui/input_dialog.hpp"
#include <gtk/gtk.h>

struct InputDialogWidgets {
    GtkWidget* window;
    GtkWidget* entry;
    GtkWidget* errorLabel;
    InputDialog::ValidatorCallback validator;
    InputDialog::ConfirmCallback onConfirm;
    InputDialog::CancelCallback onCancel;
    bool handled = false;
};

static void tryConfirm(InputDialogWidgets* w) {
    const char* text = gtk_editable_get_text(GTK_EDITABLE(w->entry));
    std::string value = text ? text : "";

    if (w->validator) {
        std::string error = w->validator(value);
        if (!error.empty()) {
            std::string markup = "<span foreground='#e74c3c'>" +
                                 std::string(g_markup_escape_text(error.c_str(), -1)) + "</span>";
            gtk_label_set_markup(GTK_LABEL(w->errorLabel), markup.c_str());
            gtk_widget_set_visible(w->errorLabel, TRUE);
            return;
        }
    }

    if (w->onConfirm) {
        std::string error = w->onConfirm(value);
        if (!error.empty()) {
            std::string markup = "<span foreground='#e74c3c'>" +
                                 std::string(g_markup_escape_text(error.c_str(), -1)) + "</span>";
            gtk_label_set_markup(GTK_LABEL(w->errorLabel), markup.c_str());
            gtk_widget_set_visible(w->errorLabel, TRUE);
            return;
        }
    }

    w->handled = true;
    gtk_window_destroy(GTK_WINDOW(w->window));
}

static void onConfirmClicked(GtkButton*, gpointer userData) {
    tryConfirm(static_cast<InputDialogWidgets*>(userData));
}

static void onCancelClicked(GtkButton*, gpointer userData) {
    auto* w = static_cast<InputDialogWidgets*>(userData);
    if (w->onCancel)
        w->onCancel();
    w->handled = true;
    gtk_window_destroy(GTK_WINDOW(w->window));
}

static void onEntryActivate(GtkEntry*, gpointer userData) {
    tryConfirm(static_cast<InputDialogWidgets*>(userData));
}

static gboolean onWindowClose(GtkWindow*, gpointer userData) {
    auto* w = static_cast<InputDialogWidgets*>(userData);
    if (!w->handled && w->onCancel)
        w->onCancel();
    return FALSE;
}

static void onWindowDestroy(GtkWidget*, gpointer userData) {
    delete static_cast<InputDialogWidgets*>(userData);
}

struct DeferredInputData {
    std::string title;
    std::string label;
    std::string initialValue;
    std::string confirmButtonText;
    InputDialog::ValidatorCallback validator;
    InputDialog::ConfirmCallback onConfirm;
    InputDialog::CancelCallback onCancel;
};

static gboolean showInputDialogIdle(gpointer userData) {
    auto* data = static_cast<DeferredInputData*>(userData);

    GtkWindow* parent = nullptr;
    if (void* host = Application::getInstance().nativeWindow())
        parent = GTK_WINDOW(host);

    GtkWidget* window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), data->title.c_str());
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 400, -1);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    }

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    if (!data->label.empty()) {
        GtkWidget* label = gtk_label_new(data->label.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_box_append(GTK_BOX(box), label);
    }

    GtkWidget* entry = gtk_entry_new();
    GtkEntryBuffer* buffer = gtk_entry_get_buffer(GTK_ENTRY(entry));
    gtk_entry_buffer_set_text(buffer, data->initialValue.c_str(), -1);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget* errorLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(errorLabel), 0);
    gtk_label_set_wrap(GTK_LABEL(errorLabel), TRUE);
    gtk_widget_set_visible(errorLabel, FALSE);
    gtk_box_append(GTK_BOX(box), errorLabel);

    GtkWidget* buttonBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttonBox, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttonBox, 8);

    GtkWidget* cancelButton = gtk_button_new_with_label("Cancel");
    GtkWidget* confirmButton = gtk_button_new_with_label(data->confirmButtonText.c_str());
    gtk_widget_add_css_class(confirmButton, "suggested-action");

    gtk_box_append(GTK_BOX(buttonBox), cancelButton);
    gtk_box_append(GTK_BOX(buttonBox), confirmButton);
    gtk_box_append(GTK_BOX(box), buttonBox);

    gtk_window_set_child(GTK_WINDOW(window), box);

    auto* widgets = new InputDialogWidgets{window,
                                           entry,
                                           errorLabel,
                                           std::move(data->validator),
                                           std::move(data->onConfirm),
                                           std::move(data->onCancel),
                                           false};

    g_signal_connect(confirmButton, "clicked", G_CALLBACK(onConfirmClicked), widgets);
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(onCancelClicked), widgets);
    g_signal_connect(entry, "activate", G_CALLBACK(onEntryActivate), widgets);
    g_signal_connect(window, "close-request", G_CALLBACK(onWindowClose), widgets);
    g_signal_connect(window, "destroy", G_CALLBACK(onWindowDestroy), widgets);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);

    delete data;
    return G_SOURCE_REMOVE;
}

void InputDialog::show(const std::string& title, const std::string& label,
                       const std::string& initialValue, const std::string& confirmButtonText,
                       ConfirmCallback onConfirm, CancelCallback onCancel,
                       ValidatorCallback validator) {
    auto* data = new DeferredInputData{title,
                                       label,
                                       initialValue,
                                       confirmButtonText,
                                       std::move(validator),
                                       std::move(onConfirm),
                                       std::move(onCancel)};
    g_idle_add(showInputDialogIdle, data);
}

#endif
