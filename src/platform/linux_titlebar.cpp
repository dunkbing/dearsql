#if defined(__linux__)

#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "platform/alert.hpp"
#include "platform/titlebar.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "ui/connection_dialog.hpp"
#include "ui/input_dialog.hpp"
#include "ui/settings_dialog.hpp"
#include <format>
#include <gtk/gtk.h>

// ---- LinuxTitlebar ----

LinuxTitlebar::LinuxTitlebar(Application* app, GtkWidget* parentWindow)
    : app_(app), parentWindow_(parentWindow) {}

void LinuxTitlebar::setup() {
    headerBar_ = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(headerBar_), TRUE);

    // sidebar toggle button
    sidebarButton_ = gtk_button_new_from_icon_name("sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(sidebarButton_, "Toggle Sidebar");
    g_signal_connect(sidebarButton_, "clicked", G_CALLBACK(onSidebarToggle), this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar_), sidebarButton_);

    // add connection button
    addButton_ = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(addButton_, "Add Database Connection");
    g_signal_connect(addButton_, "clicked", G_CALLBACK(onAddConnection), this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar_), addButton_);

    // workspace popover button (packed later, after menu button)
    workspaceButton_ = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(workspaceButton_), "view-grid-symbolic");
    gtk_widget_set_tooltip_text(workspaceButton_, "Select Workspace");

    workspacePopover_ = gtk_popover_new();
    GtkWidget* wsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(wsBox, 8);
    gtk_widget_set_margin_end(wsBox, 8);
    gtk_widget_set_margin_top(wsBox, 8);
    gtk_widget_set_margin_bottom(wsBox, 8);
    gtk_widget_set_size_request(wsBox, 200, -1);

    GtkWidget* wsLabel = gtk_label_new("Workspaces");
    gtk_widget_set_halign(wsLabel, GTK_ALIGN_START);
    gtk_widget_add_css_class(wsLabel, "dim-label");
    gtk_box_append(GTK_BOX(wsBox), wsLabel);

    workspaceItemsBox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(wsBox), workspaceItemsBox_);

    GtkWidget* wsSep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(wsBox), wsSep);

    GtkWidget* newWsButton = gtk_button_new();
    gtk_widget_add_css_class(newWsButton, "flat");
    gtk_widget_set_halign(newWsButton, GTK_ALIGN_FILL);
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("list-add-symbolic"));
        GtkWidget* lbl = gtk_label_new("New Workspace...");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(newWsButton), box);
    }
    g_signal_connect(newWsButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         gtk_popover_popdown(GTK_POPOVER(self->workspacePopover_));
                         if (!self->app_->canAddWorkspace()) {
                             Alert::show("Workspace Limit Reached",
                                         "Free tier is limited to 2 workspaces. "
                                         "Activate a license to create more.");
                             return;
                         }
                         self->showCreateWorkspaceDialog();
                     }),
                     this);
    gtk_box_append(GTK_BOX(wsBox), newWsButton);

    gtk_popover_set_child(GTK_POPOVER(workspacePopover_), wsBox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(workspaceButton_), workspacePopover_);

    g_signal_connect(workspacePopover_, "show", G_CALLBACK(+[](GtkWidget*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         self->updateWorkspaceDropdown();
                     }),
                     this);

    // main menu button — opens the shared ImGui settings dialog
    menuButton_ = gtk_button_new_from_icon_name("open-menu-symbolic");
    gtk_widget_set_tooltip_text(menuButton_, "Settings");
    g_signal_connect(menuButton_, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer) { SettingsDialog::instance().open(); }),
                     nullptr);

    // update available button (initially hidden)
    updateButton_ = gtk_button_new_from_icon_name("dialog-warning-symbolic");
    gtk_widget_set_visible(updateButton_, FALSE);
    g_signal_connect(updateButton_, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer) { checkForUpdates(); }), nullptr);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), menuButton_);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), workspaceButton_);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), updateButton_);

    gtk_window_set_titlebar(GTK_WINDOW(parentWindow_), headerBar_);

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

void LinuxTitlebar::updateWorkspaceDropdown() {
    if (!workspaceItemsBox_ || !app_) {
        return;
    }

    // clear existing rows
    GtkWidget* child = gtk_widget_get_first_child(workspaceItemsBox_);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(workspaceItemsBox_), child);
        child = next;
    }
    workspaceIdsByIndex_.clear();

    auto workspaces = app_->getWorkspaces();
    int currentWsId = app_->getCurrentWorkspaceId();

    for (size_t i = 0; i < workspaces.size(); i++) {
        workspaceIdsByIndex_.push_back(workspaces[i].id);
        bool isCurrent = (workspaces[i].id == currentWsId);

        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);

        // checkmark for active workspace
        GtkWidget* checkIcon = gtk_image_new_from_icon_name("object-select-symbolic");
        gtk_widget_set_opacity(checkIcon, isCurrent ? 1.0 : 0.0);
        gtk_box_append(GTK_BOX(row), checkIcon);

        // workspace name button
        GtkWidget* nameButton = gtk_button_new_with_label(workspaces[i].name.c_str());
        gtk_widget_add_css_class(nameButton, "flat");
        gtk_widget_set_hexpand(nameButton, TRUE);
        gtk_widget_set_halign(nameButton, GTK_ALIGN_FILL);
        g_object_set_data(G_OBJECT(nameButton), "ws_index", GINT_TO_POINTER(static_cast<int>(i)));
        g_signal_connect(
            nameButton, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer userData) {
                auto* self = static_cast<LinuxTitlebar*>(userData);
                if (self->interactionCb_)
                    self->interactionCb_();
                int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "ws_index"));
                if (idx >= 0 && idx < static_cast<int>(self->workspaceIdsByIndex_.size())) {
                    self->app_->setCurrentWorkspace(self->workspaceIdsByIndex_[idx]);
                }
                gtk_popover_popdown(GTK_POPOVER(self->workspacePopover_));
            }),
            this);
        gtk_box_append(GTK_BOX(row), nameButton);

        // edit button (visible on row hover)
        GtkWidget* editButton = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_add_css_class(editButton, "flat");
        gtk_widget_add_css_class(editButton, "ws-action-btn");
        gtk_widget_set_opacity(editButton, 0.0);
        gtk_widget_set_tooltip_text(editButton, "Rename Workspace");
        g_object_set_data(G_OBJECT(editButton), "ws_index", GINT_TO_POINTER(static_cast<int>(i)));

        g_signal_connect(
            editButton, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer userData) {
                auto* self = static_cast<LinuxTitlebar*>(userData);
                int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "ws_index"));
                if (idx < 0 || idx >= static_cast<int>(self->workspaceIdsByIndex_.size()))
                    return;
                int wsId = self->workspaceIdsByIndex_[idx];
                auto workspaces = self->app_->getWorkspaces();
                std::string currentName;
                for (const auto& ws : workspaces) {
                    if (ws.id == wsId) {
                        currentName = ws.name;
                        break;
                    }
                }
                gtk_popover_popdown(GTK_POPOVER(self->workspacePopover_));
                InputDialog::show("Rename Workspace", "", currentName, "Rename",
                                  [self, wsId](const std::string& newName) -> std::string {
                                      if (newName.empty())
                                          return "Name cannot be empty.";
                                      if (self->app_)
                                          self->app_->renameWorkspace(wsId, newName);
                                      return "";
                                  });
            }),
            this);
        gtk_box_append(GTK_BOX(row), editButton);

        // trash button (visible on row hover)
        GtkWidget* trashButton = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(trashButton, "flat");
        gtk_widget_add_css_class(trashButton, "ws-trash-btn");
        gtk_widget_set_opacity(trashButton, 0.0);
        gtk_widget_set_tooltip_text(trashButton, "Delete Workspace");
        g_object_set_data(G_OBJECT(trashButton), "ws_index", GINT_TO_POINTER(static_cast<int>(i)));

        g_signal_connect(
            trashButton, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer userData) {
                auto* self = static_cast<LinuxTitlebar*>(userData);
                int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "ws_index"));
                if (idx < 0 || idx >= static_cast<int>(self->workspaceIdsByIndex_.size()))
                    return;
                int wsId = self->workspaceIdsByIndex_[idx];
                gtk_popover_popdown(GTK_POPOVER(self->workspacePopover_));
                Alert::show("Delete Workspace",
                            "Are you sure you want to delete this workspace? "
                            "All connections in this workspace will be removed.",
                            {AlertButton{"Delete",
                                         [self, wsId]() {
                                             if (self->app_) {
                                                 self->app_->deleteWorkspace(wsId);
                                             }
                                         },
                                         AlertButton::Style::Destructive},
                             AlertButton{"Cancel", {}, AlertButton::Style::Cancel}});
            }),
            this);
        gtk_box_append(GTK_BOX(row), trashButton);

        // hover controller to show/hide action buttons and highlight row
        GtkEventController* motionCtrl = gtk_event_controller_motion_new();
        g_object_set_data(G_OBJECT(motionCtrl), "edit_btn", editButton);
        g_object_set_data(G_OBJECT(motionCtrl), "trash_btn", trashButton);
        g_signal_connect(motionCtrl, "enter",
                         G_CALLBACK(+[](GtkEventControllerMotion* ctrl, double, double, gpointer) {
                             gtk_widget_set_opacity(
                                 GTK_WIDGET(g_object_get_data(G_OBJECT(ctrl), "edit_btn")), 1.0);
                             gtk_widget_set_opacity(
                                 GTK_WIDGET(g_object_get_data(G_OBJECT(ctrl), "trash_btn")), 1.0);
                         }),
                         nullptr);
        g_signal_connect(
            motionCtrl, "leave", G_CALLBACK(+[](GtkEventControllerMotion* ctrl, gpointer) {
                gtk_widget_set_opacity(GTK_WIDGET(g_object_get_data(G_OBJECT(ctrl), "edit_btn")),
                                       0.0);
                gtk_widget_set_opacity(GTK_WIDGET(g_object_get_data(G_OBJECT(ctrl), "trash_btn")),
                                       0.0);
            }),
            nullptr);
        gtk_widget_add_controller(row, motionCtrl);

        gtk_box_append(GTK_BOX(workspaceItemsBox_), row);
    }

    // update the button label to show current workspace name
    auto currentName = app_->getCurrentWorkspaceName();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(workspaceButton_), currentName.c_str());
}

void LinuxTitlebar::applyTheme(bool isDark) {
    if (!parentWindow_)
        return;

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

void LinuxTitlebar::showCreateWorkspaceDialog() {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Create New Workspace");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parentWindow_));
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
    g_object_set_data(G_OBJECT(dialog), "titlebar", this);

    g_signal_connect(cancelButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer dlg) {
                         auto* self = static_cast<LinuxTitlebar*>(
                             g_object_get_data(G_OBJECT(dlg), "titlebar"));
                         self->updateWorkspaceDropdown();
                         gtk_window_destroy(GTK_WINDOW(dlg));
                     }),
                     dialog);

    g_signal_connect(dialog, "close-request", G_CALLBACK(+[](GtkWindow* win, gpointer) -> gboolean {
                         auto* self = static_cast<LinuxTitlebar*>(
                             g_object_get_data(G_OBJECT(win), "titlebar"));
                         self->updateWorkspaceDropdown();
                         return FALSE;
                     }),
                     nullptr);

    g_signal_connect(createButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer dlg) {
                         auto* self = static_cast<LinuxTitlebar*>(
                             g_object_get_data(G_OBJECT(dlg), "titlebar"));
                         GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "entry"));

                         const char* name = gtk_editable_get_text(GTK_EDITABLE(entry));
                         if (name && strlen(name) > 0 && self->app_) {
                             self->app_->createWorkspace(std::string(name));
                         }
                         gtk_window_destroy(GTK_WINDOW(dlg));
                     }),
                     dialog);

    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

// ---- static callbacks ----

void LinuxTitlebar::onSidebarToggle(GtkButton*, gpointer userData) {
    auto* self = static_cast<LinuxTitlebar*>(userData);
    if (self->interactionCb_)
        self->interactionCb_();
    if (self->app_) {
        self->app_->setSidebarVisible(!self->app_->isSidebarVisible());
    }
}

void LinuxTitlebar::onAddConnection(GtkButton*, gpointer userData) {
    auto* self = static_cast<LinuxTitlebar*>(userData);
    if (self->interactionCb_)
        self->interactionCb_();
    if (!self->app_)
        return;
    if (!self->app_->canAddConnection()) {
        Alert::show("Connection Limit Reached",
                    "Free tier is limited to 3 connections. Activate a license to add more.");
        return;
    }
    ConnectionDialog::instance().show(self->app_);
}

#endif // defined(__linux__)
