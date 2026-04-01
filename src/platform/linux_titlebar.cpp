#if defined(__linux__)

#include "application.hpp"
#include "config.hpp"
#include "license/license_manager.hpp"
#include "platform/alert.hpp"
#include "platform/connection_dialog.hpp"
#include "platform/titlebar.hpp"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "ui/input_dialog.hpp"
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
                                         "Free tier is limited to 1 workspace. "
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

    // main menu button (hamburger menu)
    menuButton_ = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton_), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menuButton_, "Main Menu");

    // popover content
    menuPopover_ = gtk_popover_new();
    GtkWidget* menuBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(menuBox, 12);
    gtk_widget_set_margin_end(menuBox, 12);
    gtk_widget_set_margin_top(menuBox, 12);
    gtk_widget_set_margin_bottom(menuBox, 12);
    gtk_widget_set_size_request(menuBox, 180, -1);

    // theme section
    GtkWidget* themeLabel = gtk_label_new("Theme");
    gtk_widget_set_halign(themeLabel, GTK_ALIGN_START);
    gtk_widget_add_css_class(themeLabel, "dim-label");
    gtk_box_append(GTK_BOX(menuBox), themeLabel);

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

    // font size section
    GtkWidget* fontSizeHeaderLabel = gtk_label_new("Font Size");
    gtk_widget_set_halign(fontSizeHeaderLabel, GTK_ALIGN_START);
    gtk_widget_add_css_class(fontSizeHeaderLabel, "dim-label");
    gtk_box_append(GTK_BOX(menuBox), fontSizeHeaderLabel);

    GtkWidget* fontSizeBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(fontSizeBox, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(fontSizeBox, "linked");

    GtkWidget* fontDecButton = gtk_button_new_with_label("A-");
    gtk_widget_set_tooltip_text(fontDecButton, "Decrease Font Size");
    gtk_widget_set_hexpand(fontDecButton, TRUE);
    g_signal_connect(fontDecButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         if (self->interactionCb_)
                             self->interactionCb_();
                         if (self->app_) {
                             self->app_->setFontScale(self->app_->getFontScale() - 0.1f);
                             auto label = std::format(
                                 "{}%", static_cast<int>(self->app_->getFontScale() * 100));
                             gtk_label_set_text(GTK_LABEL(self->fontSizeLabel_), label.c_str());
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(fontSizeBox), fontDecButton);

    auto fontLabel = std::format("{}%", app_ ? static_cast<int>(app_->getFontScale() * 100) : 100);
    fontSizeLabel_ = gtk_label_new(fontLabel.c_str());
    gtk_widget_set_hexpand(fontSizeLabel_, TRUE);
    gtk_box_append(GTK_BOX(fontSizeBox), fontSizeLabel_);

    GtkWidget* fontIncButton = gtk_button_new_with_label("A+");
    gtk_widget_set_tooltip_text(fontIncButton, "Increase Font Size");
    gtk_widget_set_hexpand(fontIncButton, TRUE);
    g_signal_connect(fontIncButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         if (self->interactionCb_)
                             self->interactionCb_();
                         if (self->app_) {
                             self->app_->setFontScale(self->app_->getFontScale() + 0.1f);
                             auto label = std::format(
                                 "{}%", static_cast<int>(self->app_->getFontScale() * 100));
                             gtk_label_set_text(GTK_LABEL(self->fontSizeLabel_), label.c_str());
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(fontSizeBox), fontIncButton);

    gtk_box_append(GTK_BOX(menuBox), fontSizeBox);

    // separator
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(menuBox), separator);

    // action buttons
    GtkWidget* actionBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // license button
    licenseButton_ = gtk_button_new();
    gtk_widget_add_css_class(licenseButton_, "flat");
    gtk_widget_set_halign(licenseButton_, GTK_ALIGN_FILL);
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("dialog-password-symbolic"));
        GtkWidget* lbl = gtk_label_new("Manage License");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(licenseButton_), box);
    }
    g_signal_connect(licenseButton_, "clicked", G_CALLBACK(onLicenseClicked), this);
    gtk_box_append(GTK_BOX(actionBox), licenseButton_);

    // check for updates button
    GtkWidget* checkUpdatesButton = gtk_button_new();
    gtk_widget_add_css_class(checkUpdatesButton, "flat");
    gtk_widget_set_halign(checkUpdatesButton, GTK_ALIGN_FILL);
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(box),
                       gtk_image_new_from_icon_name("software-update-available-symbolic"));
        GtkWidget* lbl = gtk_label_new("Check for Updates...");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(checkUpdatesButton), box);
    }
    g_signal_connect(checkUpdatesButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         gtk_popover_popdown(GTK_POPOVER(self->menuPopover_));
                         checkForUpdates();
                     }),
                     this);
    gtk_box_append(GTK_BOX(actionBox), checkUpdatesButton);

    // report bug button
    GtkWidget* reportBugButton = gtk_button_new();
    gtk_widget_add_css_class(reportBugButton, "flat");
    gtk_widget_set_halign(reportBugButton, GTK_ALIGN_FILL);
    {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("dialog-warning-symbolic"));
        GtkWidget* lbl = gtk_label_new("Report Bug...");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_button_set_child(GTK_BUTTON(reportBugButton), box);
    }
    g_signal_connect(reportBugButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         gtk_popover_popdown(GTK_POPOVER(self->menuPopover_));
                         std::string url =
                             "https://github.com/dunkbing/dearsql/issues/new?labels=bug"
                             "&title=%5BBug%5D+&body=%23%23+Description%0A%0A%23%23+Steps+"
                             "to+Reproduce%0A1.+%0A2.+%0A3.+%0A%0A%23%23+Expected+Behavior"
                             "%0A%0A%23%23+Actual+Behavior%0A%0A%23%23+Environment%0A-+**OS"
                             "**%3A+Linux%0A-+**DearSQL+version**%3A+" APP_VERSION
                             "%0A-+**Database**%3A+";
                         GtkUriLauncher* launcher = gtk_uri_launcher_new(url.c_str());
                         gtk_uri_launcher_launch(launcher, GTK_WINDOW(self->parentWindow_), nullptr,
                                                 nullptr, nullptr);
                         g_object_unref(launcher);
                     }),
                     this);
    gtk_box_append(GTK_BOX(actionBox), reportBugButton);

    gtk_box_append(GTK_BOX(menuBox), actionBox);

    gtk_popover_set_child(GTK_POPOVER(menuPopover_), menuBox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menuButton_), menuPopover_);

    // update theme and font label on popover show
    g_signal_connect(menuPopover_, "show", G_CALLBACK(+[](GtkWidget*, gpointer userData) {
                         auto* self = static_cast<LinuxTitlebar*>(userData);
                         self->updateThemeButtons();
                         if (self->fontSizeLabel_ && self->app_) {
                             auto label = std::format(
                                 "{}%", static_cast<int>(self->app_->getFontScale() * 100));
                             gtk_label_set_text(GTK_LABEL(self->fontSizeLabel_), label.c_str());
                         }
                     }),
                     this);

    // update available button (initially hidden)
    updateButton_ = gtk_button_new_from_icon_name("dialog-warning-symbolic");
    gtk_widget_set_visible(updateButton_, FALSE);
    g_signal_connect(updateButton_, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer) { checkForUpdates(); }), nullptr);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), menuButton_);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), workspaceButton_);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar_), updateButton_);

    gtk_window_set_titlebar(GTK_WINDOW(parentWindow_), headerBar_);
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
                      mantle + "; color: " + text +
                      "; }\n"

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

    gtk_css_provider_load_from_string(themeProvider, css.c_str());
}

void LinuxTitlebar::updateThemeButtons() {
    if (!app_ || !themeLightButton_ || !themeDarkButton_ || !themeAutoButton_) {
        return;
    }

    bool isDark = app_->isDarkTheme();

    gtk_widget_remove_css_class(themeLightButton_, "suggested-action");
    gtk_widget_remove_css_class(themeDarkButton_, "suggested-action");
    gtk_widget_remove_css_class(themeAutoButton_, "suggested-action");

    if (isDark) {
        gtk_widget_add_css_class(themeDarkButton_, "suggested-action");
    } else {
        gtk_widget_add_css_class(themeLightButton_, "suggested-action");
    }
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
        GtkWidget* linkButton = gtk_link_button_new_with_label(
            "https://buy.polar.sh/polar_cl_IpYdAWiNljfzsXgatypm2mg40Mm2c4hB0DcVX1L9P6p",
            "Purchase one");
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
    showConnectionDialog(self->app_);
}

void LinuxTitlebar::onThemeLightClicked(GtkButton*, gpointer userData) {
    auto* self = static_cast<LinuxTitlebar*>(userData);
    if (self->interactionCb_)
        self->interactionCb_();
    if (self->app_) {
        self->app_->setDarkTheme(false);
    }
    self->updateThemeButtons();
    self->applyTheme(false);
}

void LinuxTitlebar::onThemeDarkClicked(GtkButton*, gpointer userData) {
    auto* self = static_cast<LinuxTitlebar*>(userData);
    if (self->interactionCb_)
        self->interactionCb_();
    if (self->app_) {
        self->app_->setDarkTheme(true);
    }
    self->updateThemeButtons();
    self->applyTheme(true);
}

void LinuxTitlebar::onThemeAutoClicked(GtkButton*, gpointer userData) {
    auto* self = static_cast<LinuxTitlebar*>(userData);
    if (self->interactionCb_)
        self->interactionCb_();
    bool systemIsDark = false;
    if (self->app_) {
        GtkSettings* settings = gtk_settings_get_default();
        gchar* themeName = nullptr;
        g_object_get(settings, "gtk-theme-name", &themeName, nullptr);
        systemIsDark = themeName && g_str_has_suffix(themeName, "-dark");
        g_free(themeName);
        self->app_->setDarkTheme(systemIsDark);
    }
    self->updateThemeButtons();
    self->applyTheme(systemIsDark);
}

void LinuxTitlebar::onLicenseClicked(GtkButton*, gpointer userData) {
    auto* self = static_cast<LinuxTitlebar*>(userData);
    if (self->interactionCb_)
        self->interactionCb_();
    gtk_popover_popdown(GTK_POPOVER(self->menuPopover_));
    self->showLicenseDialog();
}

#endif // defined(__linux__)
