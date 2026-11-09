#if defined(__linux__)

#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "mydear/title_bar.hpp"
#include "platform/alert.hpp"
#include "platform/titlebar.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/title_bar.hpp"
#include <format>
#include <gtk/gtk.h>

// ---- LinuxTitlebar ----

LinuxTitlebar::LinuxTitlebar(Application* app, GtkWidget* parentWindow)
    : app_(app), parentWindow_(parentWindow) {}

void LinuxTitlebar::setup() {
    // native-only actions the shared settings dialog can't do itself
    SettingsDialog::instance().onManageLicense = [this]() { showLicenseDialog(); };
    SettingsDialog::instance().onReportBug = [this]() {
        constexpr const char* url =
            "https://github.com/dunkbing/dearsql/issues/new?labels=bug&title=%5BBug%5D";
#if GTK_CHECK_VERSION(4, 10, 0)
        GtkUriLauncher* launcher = gtk_uri_launcher_new(url);
        gtk_uri_launcher_launch(launcher, GTK_WINDOW(parentWindow_), nullptr, nullptr, nullptr);
        g_object_unref(launcher);
#else
        gtk_show_uri(GTK_WINDOW(parentWindow_), url, GDK_CURRENT_TIME);
#endif
    };

    applyTheme(app_ ? app_->isDarkTheme() : false);
}

void LinuxTitlebar::render() {
    TitleBarView::render(*app_);
}

float LinuxTitlebar::getHeight() const {
    return mydear::TitleBarHeight();
}

void LinuxTitlebar::applyTheme(bool isDark) {
    if (!parentWindow_)
        return;

    (void)isDark; // mydear sets prefer-dark from the theme

    static GtkCssProvider* themeProvider = nullptr;
    if (!themeProvider) {
        themeProvider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(themeProvider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    const auto& colors = Application::getInstance().getCurrentColors();

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
    std::string surface2 = toHex(colors.surface2);
    std::string overlay0 = toHex(colors.overlay0);

    std::string css = "window.dearsql-main { background: " + base +
                      "; }\n"

                      "headerbar { background: " +
                      base + "; color: " + text + "; border-bottom: 1px solid " + overlay0 +
                      "; box-shadow: none; }\n"

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

                      "headerbar button image { color: " +
                      text +
                      "; -gtk-icon-filter: none; }\n"

                      "headerbar menubutton > button {"
                      "  color: " +
                      text +
                      ";"
                      "  background: transparent;"
                      "  border-color: transparent;"
                      "}\n"
                      "headerbar menubutton > button:hover {"
                      "  background: " +
                      surface0 +
                      ";"
                      "}\n"

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

                      "popover > contents {"
                      "  background: " +
                      surface0 +
                      ";"
                      "  color: " +
                      text +
                      ";"
                      "  border: 1px solid " +
                      overlay0 +
                      ";"
                      "  border-radius: 6px;"
                      "}\n"
                      "popover > arrow {"
                      "  background: " +
                      surface0 +
                      ";"
                      "  border-color: " +
                      overlay0 +
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
                      "popover button:not(.flat):hover { background: " +
                      surface1 +
                      "; }\n"
                      "popover button.flat {"
                      "  border: none;"
                      "  border-radius: 6px;"
                      "  padding: 6px 8px;"
                      "  margin: 0;"
                      "  transition: none;"
                      "}\n"
                      "popover button.flat:hover {"
                      "  background: " +
                      surface1 +
                      ";"
                      "}\n"
                      "popover button.flat:active {"
                      "  background: " +
                      surface2 +
                      ";"
                      "}\n"
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

                      "popover button.ws-action-btn,"
                      "popover button.ws-trash-btn {"
                      "  padding: 2px;"
                      "  min-width: 0;"
                      "  min-height: 0;"
                      "}\n"
                      "popover button.ws-action-btn:hover,"
                      "popover button.ws-action-btn:hover image {"
                      "  color: " +
                      toHex(colors.blue) +
                      ";"
                      "  -gtk-icon-filter: none;"
                      "}\n"
                      "popover button.ws-trash-btn:hover,"
                      "popover button.ws-trash-btn:hover image {"
                      "  color: " +
                      toHex(colors.peach) +
                      ";"
                      "  -gtk-icon-filter: none;"
                      "}\n";

#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(themeProvider, css.c_str());
#else
    gtk_css_provider_load_from_data(themeProvider, css.c_str(), -1);
#endif
}

void LinuxTitlebar::showLicenseDialog() {
    auto& licenseManager = LicenseManager::instance();

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Manage License");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parentWindow_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget* mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(mainBox, 24);
    gtk_widget_set_margin_end(mainBox, 24);
    gtk_widget_set_margin_top(mainBox, 24);
    gtk_widget_set_margin_bottom(mainBox, 24);

    GtkWidget* statusLabel = gtk_label_new("");
    gtk_widget_set_halign(statusLabel, GTK_ALIGN_START);

    if (licenseManager.hasValidLicense()) {
        const auto info = licenseManager.getLicenseInfo();

        std::string maskedKey = info.licenseKey;
        if (maskedKey.length() > 8) {
            maskedKey = maskedKey.substr(0, 4) + "..." + maskedKey.substr(maskedKey.length() - 4);
        }

        GtkWidget* statusBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* statusIcon = gtk_image_new_from_icon_name("emblem-ok-symbolic");
        gtk_widget_add_css_class(statusIcon, "success");
        GtkWidget* statusText = gtk_label_new("License Active");
        gtk_widget_add_css_class(statusText, "title-3");
        gtk_box_append(GTK_BOX(statusBox), statusIcon);
        gtk_box_append(GTK_BOX(statusBox), statusText);
        gtk_box_append(GTK_BOX(mainBox), statusBox);

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
        gtk_box_append(GTK_BOX(mainBox), statusLabel);

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

        gtk_box_append(GTK_BOX(mainBox), statusLabel);

        GtkWidget* linkBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(linkBox, GTK_ALIGN_START);
        GtkWidget* linkText = gtk_label_new("Don't have a license?");
        gtk_widget_add_css_class(linkText, "dim-label");
        GtkWidget* linkButton = gtk_link_button_new_with_label(PURCHASE_URL, "Purchase one");
        gtk_box_append(GTK_BOX(linkBox), linkText);
        gtk_box_append(GTK_BOX(linkBox), linkButton);
        gtk_box_append(GTK_BOX(mainBox), linkBox);

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

#endif // defined(__linux__)
