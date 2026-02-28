#if defined(__linux__)

#include "platform/linux_connection_dialog.hpp"
#include "app_state.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/query_executor.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "platform/linux_platform.hpp"
#include "utils/file_dialog.hpp"
#include <atomic>
#include <cstring>
#include <gtk/gtk.h>
#include <thread>

// ---------------------------------------------------------------------------
// Internal state structs
// ---------------------------------------------------------------------------

struct ConnectionDialogData {
    Application* app = nullptr;
    std::shared_ptr<DatabaseInterface> editingDb;
    int editingConnectionId = -1;
    std::atomic<bool> cancelled{false};

    GtkWidget* dialog = nullptr;
    GtkWidget* nameEntry = nullptr;
    GtkWidget* typeDropdown = nullptr;
    GtkWidget* fieldsBox = nullptr;

    // SQLite
    GtkWidget* sqlitePathEntry = nullptr;

    // Server
    GtkWidget* hostEntry = nullptr;
    GtkWidget* portEntry = nullptr;
    GtkWidget* databaseEntry = nullptr;

    // SSL
    GtkWidget* sslModeDropdown = nullptr;

    // Auth
    GtkWidget* authPasswordRadio = nullptr;
    GtkWidget* authNoneRadio = nullptr;
    GtkWidget* usernameEntry = nullptr;
    GtkWidget* passwordEntry = nullptr;
    GtkWidget* credentialsRow = nullptr;

    // Show all databases
    GtkWidget* showAllDbsCheck = nullptr;

    // Bottom
    GtkWidget* statusLabel = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* connectButton = nullptr;
};

struct CreateDatabaseDialogData {
    Application* app = nullptr;
    std::shared_ptr<DatabaseInterface> db;

    GtkWidget* dialog = nullptr;
    GtkWidget* nameEntry = nullptr;
    GtkWidget* commentEntry = nullptr;

    // PostgreSQL
    GtkWidget* ownerDropdown = nullptr;
    GtkWidget* templateDropdown = nullptr;
    GtkWidget* encodingDropdown = nullptr;
    GtkWidget* tablespaceDropdown = nullptr;

    // MySQL
    GtkWidget* charsetDropdown = nullptr;
    GtkWidget* collationDropdown = nullptr;

    GtkWidget* statusLabel = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* createButton = nullptr;
};

// ---------------------------------------------------------------------------
// Singleton guards
// ---------------------------------------------------------------------------

static GtkWidget* sActiveConnectionDialog = nullptr;
static GtkWidget* sActiveCreateDatabaseDialog = nullptr;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr const char* kSslModes[] = {"disable", "allow",     "prefer",
                                            "require", "verify-ca", "verify-full"};
static constexpr int kSslModeCount = 6;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GtkWidget* getParentWindow(Application* app) {
    auto* platform = dynamic_cast<LinuxPlatform*>(app->getPlatform());
    return platform ? platform->getGtkWindow() : nullptr;
}

static GtkWidget* makeLabel(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_size_request(label, 90, -1);
    return label;
}

static GtkWidget* makeEntry(const char* placeholder) {
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_hexpand(entry, TRUE);
    return entry;
}

static GtkWidget* makeRow(GtkWidget* label, GtkWidget* field) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), field);
    return row;
}

static void clearBox(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static GtkWidget* makeStringDropdown(const char* const items[], int count, int selected) {
    GtkStringList* model = gtk_string_list_new(nullptr);
    for (int i = 0; i < count; i++) {
        gtk_string_list_append(model, items[i]);
    }
    GtkWidget* dropdown = gtk_drop_down_new(G_LIST_MODEL(model), nullptr);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected >= 0 ? selected : 0);
    return dropdown;
}

// ---------------------------------------------------------------------------
// Connection dialog: field rebuilding
// ---------------------------------------------------------------------------

static void onAuthToggled(GtkCheckButton*, gpointer userData) {
    auto* data = static_cast<ConnectionDialogData*>(userData);
    bool show = gtk_check_button_get_active(GTK_CHECK_BUTTON(data->authPasswordRadio));
    gtk_widget_set_visible(data->credentialsRow, show);
}

static void rebuildFieldsForType(ConnectionDialogData* data) {
    clearBox(data->fieldsBox);

    // Reset widget pointers
    data->sqlitePathEntry = nullptr;
    data->hostEntry = nullptr;
    data->portEntry = nullptr;
    data->databaseEntry = nullptr;
    data->sslModeDropdown = nullptr;
    data->authPasswordRadio = nullptr;
    data->authNoneRadio = nullptr;
    data->usernameEntry = nullptr;
    data->passwordEntry = nullptr;
    data->credentialsRow = nullptr;
    data->showAllDbsCheck = nullptr;

    int selectedType = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->typeDropdown));
    auto type = static_cast<DatabaseType>(selectedType);

    if (type == DatabaseType::SQLITE) {
        // Path + Browse
        data->sqlitePathEntry = makeEntry("Database file path");
        GtkWidget* browseBtn = gtk_button_new_with_label("Browse...");
        g_signal_connect(
            browseBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ud) {
                auto* d = static_cast<ConnectionDialogData*>(ud);
                auto db = FileDialog::openSQLiteFile();
                if (db) {
                    auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(db);
                    if (sqliteDb) {
                        gtk_editable_set_text(GTK_EDITABLE(d->sqlitePathEntry),
                                              sqliteDb->getPath().c_str());
                        const char* name = gtk_editable_get_text(GTK_EDITABLE(d->nameEntry));
                        if (!name || strlen(name) == 0 ||
                            strcmp(name, "Untitled connection") == 0) {
                            gtk_editable_set_text(GTK_EDITABLE(d->nameEntry),
                                                  sqliteDb->getConnectionInfo().name.c_str());
                        }
                    }
                }
            }),
            data);

        GtkWidget* pathRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(pathRow), makeLabel("File"));
        gtk_box_append(GTK_BOX(pathRow), data->sqlitePathEntry);
        gtk_box_append(GTK_BOX(pathRow), browseBtn);
        gtk_box_append(GTK_BOX(data->fieldsBox), pathRow);
        return;
    }

    // --- Server fields ---

    // Host + Port
    data->hostEntry = makeEntry("localhost");
    gtk_editable_set_text(GTK_EDITABLE(data->hostEntry), "localhost");
    data->portEntry = makeEntry("5432");
    gtk_widget_set_size_request(data->portEntry, 70, -1);
    gtk_widget_set_hexpand(data->portEntry, FALSE);

    // Default port by type
    const char* defaultPort = "5432";
    if (type == DatabaseType::MYSQL || type == DatabaseType::MARIADB)
        defaultPort = "3306";
    else if (type == DatabaseType::MONGODB)
        defaultPort = "27017";
    else if (type == DatabaseType::REDIS)
        defaultPort = "6379";
    gtk_editable_set_text(GTK_EDITABLE(data->portEntry), defaultPort);

    GtkWidget* hostRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(hostRow), makeLabel("Host"));
    gtk_box_append(GTK_BOX(hostRow), data->hostEntry);
    GtkWidget* portLabel = gtk_label_new("Port");
    gtk_widget_add_css_class(portLabel, "dim-label");
    gtk_box_append(GTK_BOX(hostRow), portLabel);
    gtk_box_append(GTK_BOX(hostRow), data->portEntry);
    gtk_box_append(GTK_BOX(data->fieldsBox), hostRow);

    // Database (not for Redis)
    if (type != DatabaseType::REDIS) {
        data->databaseEntry = makeEntry("(optional)");
        GtkWidget* dbRow = makeRow(makeLabel("Database"), data->databaseEntry);
        gtk_box_append(GTK_BOX(data->fieldsBox), dbRow);
    }

    // SSL Mode (PostgreSQL only)
    if (type == DatabaseType::POSTGRESQL) {
        data->sslModeDropdown = makeStringDropdown(kSslModes, kSslModeCount, 2);
        GtkWidget* sslRow = makeRow(makeLabel("SSL Mode"), data->sslModeDropdown);
        gtk_box_append(GTK_BOX(data->fieldsBox), sslRow);
    }

    // Auth radio
    bool defaultNoAuth = (type == DatabaseType::MONGODB || type == DatabaseType::REDIS);
    data->authPasswordRadio = gtk_check_button_new_with_label("Username & Password");
    data->authNoneRadio = gtk_check_button_new_with_label("None");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(data->authNoneRadio),
                               GTK_CHECK_BUTTON(data->authPasswordRadio));
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(defaultNoAuth ? data->authNoneRadio : data->authPasswordRadio), TRUE);

    GtkWidget* authRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(authRow), makeLabel("Auth"));
    gtk_box_append(GTK_BOX(authRow), data->authPasswordRadio);
    gtk_box_append(GTK_BOX(authRow), data->authNoneRadio);
    gtk_box_append(GTK_BOX(data->fieldsBox), authRow);

    // Credentials row
    data->usernameEntry = makeEntry("Username");
    data->passwordEntry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(data->passwordEntry), TRUE);
    gtk_widget_set_hexpand(data->passwordEntry, TRUE);

    data->credentialsRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(data->credentialsRow), makeLabel("Username"));
    gtk_box_append(GTK_BOX(data->credentialsRow), data->usernameEntry);
    GtkWidget* pwLabel = gtk_label_new("Password");
    gtk_widget_add_css_class(pwLabel, "dim-label");
    gtk_box_append(GTK_BOX(data->credentialsRow), pwLabel);
    gtk_box_append(GTK_BOX(data->credentialsRow), data->passwordEntry);
    gtk_box_append(GTK_BOX(data->fieldsBox), data->credentialsRow);

    gtk_widget_set_visible(data->credentialsRow, !defaultNoAuth);

    g_signal_connect(data->authPasswordRadio, "toggled", G_CALLBACK(onAuthToggled), data);

    // Show all databases (not for Redis)
    if (type != DatabaseType::REDIS) {
        data->showAllDbsCheck = gtk_check_button_new_with_label("Show all databases");
        GtkWidget* checkRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        // Spacer to align with fields
        GtkWidget* spacer = gtk_label_new("");
        gtk_widget_set_size_request(spacer, 90, -1);
        gtk_box_append(GTK_BOX(checkRow), spacer);
        gtk_box_append(GTK_BOX(checkRow), data->showAllDbsCheck);
        gtk_box_append(GTK_BOX(data->fieldsBox), checkRow);
    }
}

// ---------------------------------------------------------------------------
// Connection dialog: async connection
// ---------------------------------------------------------------------------

struct AsyncConnectResult {
    GtkWidget* dialogRef;
    bool success;
    std::string error;
    std::shared_ptr<DatabaseInterface> db;
    DatabaseConnectionInfo info;
    Application* app;
    std::shared_ptr<DatabaseInterface> editingDb;
    int editingConnectionId;
    std::atomic<bool>* cancelledFlag;
};

static void handleConnectionSuccess(AsyncConnectResult* r) {
    if (r->editingConnectionId != -1 && r->editingDb) {
        SavedConnection conn;
        conn.id = r->editingConnectionId;
        conn.connectionInfo = r->info;
        conn.workspaceId = r->app->getCurrentWorkspaceId();
        r->app->getAppState()->updateConnection(conn);

        r->db->setConnectionId(r->editingConnectionId);
        auto& dbs = r->app->getDatabases();
        for (size_t i = 0; i < dbs.size(); i++) {
            if (dbs[i] == r->editingDb) {
                dbs[i]->disconnect();
                dbs[i] = r->db;
                break;
            }
        }
    } else {
        SavedConnection conn;
        conn.connectionInfo = r->info;
        conn.workspaceId = r->app->getCurrentWorkspaceId();
        int newId = r->app->getAppState()->saveConnection(conn);
        if (newId != -1) {
            r->db->setConnectionId(newId);
        }
        r->app->addDatabase(r->db);
    }
}

static void connectSQLite(ConnectionDialogData* data) {
    const char* path = gtk_editable_get_text(GTK_EDITABLE(data->sqlitePathEntry));
    if (!path || strlen(path) == 0) {
        gtk_label_set_text(GTK_LABEL(data->statusLabel), "Please select a database file");
        return;
    }

    const char* name = gtk_editable_get_text(GTK_EDITABLE(data->nameEntry));

    DatabaseConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.name = name ? name : "";
    info.path = path;

    auto db = std::make_shared<SQLiteDatabase>(info);
    auto [success, error] = db->connect();

    if (success) {
        if (data->editingConnectionId != -1 && data->editingDb) {
            SavedConnection conn;
            conn.id = data->editingConnectionId;
            conn.connectionInfo = info;
            conn.workspaceId = data->app->getCurrentWorkspaceId();
            data->app->getAppState()->updateConnection(conn);
            db->setConnectionId(data->editingConnectionId);
            auto& dbs = data->app->getDatabases();
            for (size_t i = 0; i < dbs.size(); i++) {
                if (dbs[i] == data->editingDb) {
                    dbs[i]->disconnect();
                    dbs[i] = db;
                    break;
                }
            }
        } else {
            SavedConnection conn;
            conn.connectionInfo = info;
            conn.workspaceId = data->app->getCurrentWorkspaceId();
            int newId = data->app->getAppState()->saveConnection(conn);
            if (newId != -1) {
                db->setConnectionId(newId);
            }
            data->app->addDatabase(db);
        }
        gtk_window_destroy(GTK_WINDOW(data->dialog));
    } else {
        gtk_label_set_text(GTK_LABEL(data->statusLabel), ("Failed: " + error).c_str());
    }
}

static void connectServerAsync(ConnectionDialogData* data) {
    const char* name = gtk_editable_get_text(GTK_EDITABLE(data->nameEntry));
    int selectedType = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->typeDropdown));
    auto type = static_cast<DatabaseType>(selectedType);

    const char* host = gtk_editable_get_text(GTK_EDITABLE(data->hostEntry));
    const char* portStr = gtk_editable_get_text(GTK_EDITABLE(data->portEntry));
    int port = portStr ? atoi(portStr) : 0;
    if (port <= 0)
        port = 1;
    if (port > 65535)
        port = 65535;

    const char* database =
        data->databaseEntry ? gtk_editable_get_text(GTK_EDITABLE(data->databaseEntry)) : "";
    bool authEnabled = data->authPasswordRadio &&
                       gtk_check_button_get_active(GTK_CHECK_BUTTON(data->authPasswordRadio));
    const char* username = (authEnabled && data->usernameEntry)
                               ? gtk_editable_get_text(GTK_EDITABLE(data->usernameEntry))
                               : "";
    const char* password = (authEnabled && data->passwordEntry)
                               ? gtk_editable_get_text(GTK_EDITABLE(data->passwordEntry))
                               : "";
    bool showAllDbs = data->showAllDbsCheck &&
                      gtk_check_button_get_active(GTK_CHECK_BUTTON(data->showAllDbsCheck));

    int sslModeIdx = data->sslModeDropdown
                         ? gtk_drop_down_get_selected(GTK_DROP_DOWN(data->sslModeDropdown))
                         : 2;

    // Validate
    if (authEnabled && (!username || strlen(username) == 0) && type != DatabaseType::MONGODB &&
        type != DatabaseType::REDIS) {
        gtk_label_set_text(GTK_LABEL(data->statusLabel), "Please enter a username");
        return;
    }

    // UI feedback
    gtk_widget_set_sensitive(data->connectButton, FALSE);
    gtk_spinner_start(GTK_SPINNER(data->spinner));
    gtk_label_set_text(GTK_LABEL(data->statusLabel), "Connecting...");

    // Capture values
    std::string nameStr = name ? name : "";
    std::string hostStr = host ? host : "";
    std::string dbStr = database ? database : "";
    std::string userStr = username ? username : "";
    std::string passStr = password ? password : "";

    Application* appPtr = data->app;
    auto editingDb = data->editingDb;
    int editConnId = data->editingConnectionId;
    GtkWidget* dialogRef = data->dialog;
    std::atomic<bool>* cancelledFlag = &data->cancelled;

    g_object_ref(dialogRef);

    std::thread([=]() {
        DatabaseConnectionInfo info;
        info.type = type;
        info.name = nameStr;
        info.host = hostStr;
        info.port = port;
        info.showAllDatabases = showAllDbs;

        if (authEnabled) {
            info.username = userStr;
            info.password = passStr;
        }

        std::shared_ptr<DatabaseInterface> db;
        switch (type) {
        case DatabaseType::POSTGRESQL:
            info.database = dbStr.empty() ? "postgres" : dbStr;
            info.sslmode = kSslModes[sslModeIdx];
            db = std::make_shared<PostgresDatabase>(info);
            break;
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB:
            info.database = dbStr.empty() ? "mysql" : dbStr;
            db = std::make_shared<MySQLDatabase>(info);
            break;
        case DatabaseType::MONGODB:
            info.database = dbStr;
            db = std::make_shared<MongoDBDatabase>(info);
            break;
        case DatabaseType::REDIS:
            db = std::make_shared<RedisDatabase>(info);
            break;
        default:
            break;
        }

        bool success = false;
        std::string errorMsg;
        if (db) {
            auto [s, e] = db->connect();
            success = s;
            errorMsg = e;
        } else {
            errorMsg = "Failed to create database connection";
        }

        auto* result = new AsyncConnectResult{dialogRef, success,   errorMsg,   db,           info,
                                              appPtr,    editingDb, editConnId, cancelledFlag};

        g_idle_add(
            +[](gpointer ud) -> gboolean {
                auto* r = static_cast<AsyncConnectResult*>(ud);

                if (r->cancelledFlag->load()) {
                    if (r->success && r->db) {
                        r->db->disconnect();
                    }
                } else if (r->success) {
                    handleConnectionSuccess(r);
                    gtk_window_destroy(GTK_WINDOW(r->dialogRef));
                } else {
                    auto* d = static_cast<ConnectionDialogData*>(
                        g_object_get_data(G_OBJECT(r->dialogRef), "data"));
                    if (d) {
                        gtk_label_set_text(GTK_LABEL(d->statusLabel),
                                           ("Failed: " + r->error).c_str());
                        gtk_widget_set_sensitive(d->connectButton, TRUE);
                        gtk_spinner_stop(GTK_SPINNER(d->spinner));
                    }
                }

                g_object_unref(r->dialogRef);
                delete r;
                return G_SOURCE_REMOVE;
            },
            result);
    }).detach();
}

// ---------------------------------------------------------------------------
// Connection dialog: build & show
// ---------------------------------------------------------------------------

static void destroyConnectionDialogData(gpointer ptr) {
    auto* data = static_cast<ConnectionDialogData*>(ptr);
    data->editingDb.reset();
    delete data;
}

static GtkWidget* buildConnectionDialog(ConnectionDialogData* data) {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Connect to Database");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 500, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget* parent = getParentWindow(data->app);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
    }

    data->dialog = dialog;

    GtkWidget* mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(mainBox, 24);
    gtk_widget_set_margin_end(mainBox, 24);
    gtk_widget_set_margin_top(mainBox, 24);
    gtk_widget_set_margin_bottom(mainBox, 24);

    // Name row
    data->nameEntry = makeEntry("Untitled connection");
    gtk_editable_set_text(GTK_EDITABLE(data->nameEntry), "Untitled connection");
    GtkWidget* nameRow = makeRow(makeLabel("Name"), data->nameEntry);
    gtk_box_append(GTK_BOX(mainBox), nameRow);

    // Type dropdown
    static const char* typeNames[] = {"SQLite",  "PostgreSQL", "MySQL",
                                      "MariaDB", "Redis",      "MongoDB"};
    data->typeDropdown = makeStringDropdown(typeNames, 6, 0);
    GtkWidget* typeRow = makeRow(makeLabel("Type"), data->typeDropdown);
    gtk_box_append(GTK_BOX(mainBox), typeRow);

    // Separator
    GtkWidget* sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(mainBox), sep1);

    // Dynamic fields container
    data->fieldsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_append(GTK_BOX(mainBox), data->fieldsBox);

    // Type change handler
    g_signal_connect(data->typeDropdown, "notify::selected",
                     G_CALLBACK(+[](GtkDropDown*, GParamSpec*, gpointer ud) {
                         auto* d = static_cast<ConnectionDialogData*>(ud);
                         rebuildFieldsForType(d);
                         gtk_label_set_text(GTK_LABEL(d->statusLabel), "");
                     }),
                     data);

    // Status + spinner row
    GtkWidget* statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    data->statusLabel = gtk_label_new("");
    gtk_widget_set_hexpand(data->statusLabel, TRUE);
    gtk_widget_set_halign(data->statusLabel, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(data->statusLabel), TRUE);
    data->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(statusRow), data->statusLabel);
    gtk_box_append(GTK_BOX(statusRow), data->spinner);
    gtk_box_append(GTK_BOX(mainBox), statusRow);

    // Separator
    GtkWidget* sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(mainBox), sep2);

    // Button row
    GtkWidget* btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btnRow, GTK_ALIGN_END);

    GtkWidget* cancelBtn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancelBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ud) {
                         auto* d = static_cast<ConnectionDialogData*>(ud);
                         gtk_window_destroy(GTK_WINDOW(d->dialog));
                     }),
                     data);

    data->connectButton = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(data->connectButton, "suggested-action");
    g_signal_connect(
        data->connectButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ud) {
            auto* d = static_cast<ConnectionDialogData*>(ud);

            const char* name = gtk_editable_get_text(GTK_EDITABLE(d->nameEntry));
            if (!name || strlen(name) == 0) {
                gtk_label_set_text(GTK_LABEL(d->statusLabel), "Please enter a connection name");
                return;
            }

            int selectedType = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->typeDropdown));
            auto type = static_cast<DatabaseType>(selectedType);

            if (type == DatabaseType::SQLITE) {
                connectSQLite(d);
            } else {
                connectServerAsync(d);
            }
        }),
        data);

    gtk_box_append(GTK_BOX(btnRow), cancelBtn);
    gtk_box_append(GTK_BOX(btnRow), data->connectButton);
    gtk_box_append(GTK_BOX(mainBox), btnRow);

    gtk_window_set_child(GTK_WINDOW(dialog), mainBox);

    // Attach data and close/destroy handlers
    g_object_set_data_full(G_OBJECT(dialog), "data", data, destroyConnectionDialogData);
    g_signal_connect(dialog, "close-request", G_CALLBACK(+[](GtkWindow* win, gpointer) -> gboolean {
                         auto* d = static_cast<ConnectionDialogData*>(
                             g_object_get_data(G_OBJECT(win), "data"));
                         if (d)
                             d->cancelled = true;
                         return FALSE;
                     }),
                     nullptr);
    // Clear singleton on destroy (fires for both close-request and gtk_window_destroy)
    g_signal_connect(dialog, "destroy",
                     G_CALLBACK(+[](GtkWidget*, gpointer) { sActiveConnectionDialog = nullptr; }),
                     nullptr);

    // Build initial fields
    rebuildFieldsForType(data);

    return dialog;
}

static void populateFieldsFromConnection(ConnectionDialogData* data,
                                         const std::shared_ptr<DatabaseInterface>& db) {
    const auto& info = db->getConnectionInfo();

    gtk_editable_set_text(GTK_EDITABLE(data->nameEntry), info.name.c_str());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(data->typeDropdown), static_cast<int>(info.type));

    // Rebuild fields for the correct type
    rebuildFieldsForType(data);

    switch (info.type) {
    case DatabaseType::SQLITE:
        if (data->sqlitePathEntry)
            gtk_editable_set_text(GTK_EDITABLE(data->sqlitePathEntry), info.path.c_str());
        break;

    case DatabaseType::POSTGRESQL:
        if (data->hostEntry)
            gtk_editable_set_text(GTK_EDITABLE(data->hostEntry), info.host.c_str());
        if (data->portEntry) {
            char portBuf[16];
            snprintf(portBuf, sizeof(portBuf), "%d", info.port);
            gtk_editable_set_text(GTK_EDITABLE(data->portEntry), portBuf);
        }
        if (data->databaseEntry)
            gtk_editable_set_text(GTK_EDITABLE(data->databaseEntry), info.database.c_str());
        if (data->showAllDbsCheck)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(data->showAllDbsCheck),
                                        info.showAllDatabases);
        if (data->sslModeDropdown) {
            for (int i = 0; i < kSslModeCount; i++) {
                if (info.sslmode == kSslModes[i]) {
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(data->sslModeDropdown), i);
                    break;
                }
            }
        }
        if (info.username.empty()) {
            if (data->authNoneRadio)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(data->authNoneRadio), TRUE);
        } else {
            if (data->authPasswordRadio)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(data->authPasswordRadio), TRUE);
            if (data->usernameEntry)
                gtk_editable_set_text(GTK_EDITABLE(data->usernameEntry), info.username.c_str());
            if (data->passwordEntry)
                gtk_editable_set_text(GTK_EDITABLE(data->passwordEntry), info.password.c_str());
        }
        break;

    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
    case DatabaseType::MONGODB:
        if (data->hostEntry)
            gtk_editable_set_text(GTK_EDITABLE(data->hostEntry), info.host.c_str());
        if (data->portEntry) {
            char portBuf[16];
            snprintf(portBuf, sizeof(portBuf), "%d", info.port);
            gtk_editable_set_text(GTK_EDITABLE(data->portEntry), portBuf);
        }
        if (data->databaseEntry)
            gtk_editable_set_text(GTK_EDITABLE(data->databaseEntry), info.database.c_str());
        if (data->showAllDbsCheck)
            gtk_check_button_set_active(GTK_CHECK_BUTTON(data->showAllDbsCheck),
                                        info.showAllDatabases);
        if (info.username.empty() && info.password.empty()) {
            if (data->authNoneRadio)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(data->authNoneRadio), TRUE);
        } else {
            if (data->authPasswordRadio)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(data->authPasswordRadio), TRUE);
            if (data->usernameEntry)
                gtk_editable_set_text(GTK_EDITABLE(data->usernameEntry), info.username.c_str());
            if (data->passwordEntry)
                gtk_editable_set_text(GTK_EDITABLE(data->passwordEntry), info.password.c_str());
        }
        break;

    case DatabaseType::REDIS:
        if (data->hostEntry)
            gtk_editable_set_text(GTK_EDITABLE(data->hostEntry), info.host.c_str());
        if (data->portEntry) {
            char portBuf[16];
            snprintf(portBuf, sizeof(portBuf), "%d", info.port);
            gtk_editable_set_text(GTK_EDITABLE(data->portEntry), portBuf);
        }
        if (info.username.empty() && info.password.empty()) {
            if (data->authNoneRadio)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(data->authNoneRadio), TRUE);
        } else {
            if (data->authPasswordRadio)
                gtk_check_button_set_active(GTK_CHECK_BUTTON(data->authPasswordRadio), TRUE);
            if (data->usernameEntry)
                gtk_editable_set_text(GTK_EDITABLE(data->usernameEntry), info.username.c_str());
            if (data->passwordEntry)
                gtk_editable_set_text(GTK_EDITABLE(data->passwordEntry), info.password.c_str());
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Create Database dialog
// ---------------------------------------------------------------------------

static void destroyCreateDbDialogData(gpointer ptr) {
    auto* data = static_cast<CreateDatabaseDialogData*>(ptr);
    data->db.reset();
    delete data;
}

static void populatePostgresOptions(CreateDatabaseDialogData* data) {
    if (!data->db)
        return;
    auto* executor = dynamic_cast<IQueryExecutor*>(data->db.get());
    if (!executor)
        return;

    // Owners
    try {
        auto result = executor->executeQuery("SELECT rolname FROM pg_roles ORDER BY rolname");
        if (result.success() && data->ownerDropdown) {
            auto* model =
                GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->ownerDropdown)));
            // Clear
            while (g_list_model_get_n_items(G_LIST_MODEL(model)) > 0)
                gtk_string_list_remove(model, 0);
            int pgIdx = 0;
            int idx = 0;
            for (const auto& row : result[0].tableData) {
                if (!row.empty()) {
                    gtk_string_list_append(model, row[0].c_str());
                    if (row[0] == "postgres")
                        pgIdx = idx;
                    idx++;
                }
            }
            gtk_drop_down_set_selected(GTK_DROP_DOWN(data->ownerDropdown), pgIdx);
        }
    } catch (...) {
    }

    // Templates
    try {
        auto result = executor->executeQuery(
            "SELECT datname FROM pg_database WHERE datistemplate ORDER BY datname");
        if (result.success() && data->templateDropdown) {
            auto* model =
                GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->templateDropdown)));
            while (g_list_model_get_n_items(G_LIST_MODEL(model)) > 0)
                gtk_string_list_remove(model, 0);
            gtk_string_list_append(model, "template1");
            for (const auto& row : result[0].tableData) {
                if (!row.empty() && row[0] != "template1") {
                    gtk_string_list_append(model, row[0].c_str());
                }
            }
        }
    } catch (...) {
    }

    // Tablespaces
    try {
        auto result = executor->executeQuery("SELECT spcname FROM pg_tablespace ORDER BY spcname");
        if (result.success() && data->tablespaceDropdown) {
            auto* model =
                GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->tablespaceDropdown)));
            while (g_list_model_get_n_items(G_LIST_MODEL(model)) > 0)
                gtk_string_list_remove(model, 0);
            for (const auto& row : result[0].tableData) {
                if (!row.empty()) {
                    gtk_string_list_append(model, row[0].c_str());
                }
            }
        }
    } catch (...) {
    }
}

static void updateCollationForCharset(CreateDatabaseDialogData* data) {
    if (!data->charsetDropdown || !data->collationDropdown)
        return;

    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(data->charsetDropdown));
    auto* charsetModel =
        GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->charsetDropdown)));
    const char* charset = gtk_string_list_get_string(charsetModel, idx);

    auto* model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(data->collationDropdown)));
    while (g_list_model_get_n_items(G_LIST_MODEL(model)) > 0)
        gtk_string_list_remove(model, 0);

    if (!charset)
        return;

    if (strcmp(charset, "utf8mb4") == 0) {
        for (auto c :
             {"utf8mb4_unicode_ci", "utf8mb4_0900_ai_ci", "utf8mb4_general_ci", "utf8mb4_bin"})
            gtk_string_list_append(model, c);
    } else if (strcmp(charset, "utf8mb3") == 0 || strcmp(charset, "utf8") == 0) {
        for (auto c : {"utf8_unicode_ci", "utf8_general_ci", "utf8_bin"})
            gtk_string_list_append(model, c);
    } else if (strcmp(charset, "latin1") == 0) {
        for (auto c : {"latin1_swedish_ci", "latin1_general_ci", "latin1_bin"})
            gtk_string_list_append(model, c);
    } else if (strcmp(charset, "ascii") == 0) {
        for (auto c : {"ascii_general_ci", "ascii_bin"})
            gtk_string_list_append(model, c);
    } else if (strcmp(charset, "binary") == 0) {
        gtk_string_list_append(model, "binary");
    }
}

static GtkWidget* buildCreateDatabaseDialog(CreateDatabaseDialogData* data) {
    DatabaseType type = data->db->getConnectionInfo().type;
    bool isPostgres = (type == DatabaseType::POSTGRESQL);

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog),
                         isPostgres ? "Create PostgreSQL Database" : "Create MySQL Database");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 450, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget* parent = getParentWindow(data->app);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
    }

    data->dialog = dialog;

    GtkWidget* mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(mainBox, 24);
    gtk_widget_set_margin_end(mainBox, 24);
    gtk_widget_set_margin_top(mainBox, 24);
    gtk_widget_set_margin_bottom(mainBox, 24);

    // Name
    data->nameEntry = makeEntry("new_database");
    gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Name"), data->nameEntry));

    if (isPostgres) {
        // Owner
        static const char* defOwner[] = {"postgres"};
        data->ownerDropdown = makeStringDropdown(defOwner, 1, 0);
        gtk_widget_set_hexpand(data->ownerDropdown, TRUE);
        gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Owner"), data->ownerDropdown));

        // Template
        static const char* defTemplate[] = {"template1", "template0"};
        data->templateDropdown = makeStringDropdown(defTemplate, 2, 0);
        gtk_widget_set_hexpand(data->templateDropdown, TRUE);
        gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Template"), data->templateDropdown));

        // Encoding
        static const char* encodings[] = {"UTF8",      "LATIN1",  "LATIN2",    "LATIN9", "WIN1252",
                                          "SQL_ASCII", "EUC_JP",  "EUC_KR",    "EUC_CN", "SJIS",
                                          "BIG5",      "WIN1251", "ISO_8859_5"};
        data->encodingDropdown = makeStringDropdown(encodings, 13, 0);
        gtk_widget_set_hexpand(data->encodingDropdown, TRUE);
        gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Encoding"), data->encodingDropdown));

        // Tablespace
        static const char* defTs[] = {"pg_default"};
        data->tablespaceDropdown = makeStringDropdown(defTs, 1, 0);
        gtk_widget_set_hexpand(data->tablespaceDropdown, TRUE);
        gtk_box_append(GTK_BOX(mainBox),
                       makeRow(makeLabel("Tablespace"), data->tablespaceDropdown));

        populatePostgresOptions(data);
    } else {
        // MySQL: Charset
        static const char* charsets[] = {"utf8mb4", "utf8mb3", "utf8",  "latin1", "ascii",
                                         "binary",  "utf16",   "utf32", "cp1251", "gbk",
                                         "big5",    "euckr",   "sjis"};
        data->charsetDropdown = makeStringDropdown(charsets, 13, 0);
        gtk_widget_set_hexpand(data->charsetDropdown, TRUE);
        gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Charset"), data->charsetDropdown));

        // Collation
        static const char* defCollations[] = {"utf8mb4_unicode_ci", "utf8mb4_0900_ai_ci",
                                              "utf8mb4_general_ci", "utf8mb4_bin"};
        data->collationDropdown = makeStringDropdown(defCollations, 4, 0);
        gtk_widget_set_hexpand(data->collationDropdown, TRUE);
        gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Collation"), data->collationDropdown));

        // Charset change updates collation
        g_signal_connect(data->charsetDropdown, "notify::selected",
                         G_CALLBACK(+[](GtkDropDown*, GParamSpec*, gpointer ud) {
                             updateCollationForCharset(static_cast<CreateDatabaseDialogData*>(ud));
                         }),
                         data);
    }

    // Comment
    data->commentEntry = makeEntry("(optional)");
    gtk_box_append(GTK_BOX(mainBox), makeRow(makeLabel("Comment"), data->commentEntry));

    // Status + spinner
    GtkWidget* statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    data->statusLabel = gtk_label_new("");
    gtk_widget_set_hexpand(data->statusLabel, TRUE);
    gtk_widget_set_halign(data->statusLabel, GTK_ALIGN_START);
    data->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(statusRow), data->statusLabel);
    gtk_box_append(GTK_BOX(statusRow), data->spinner);
    gtk_box_append(GTK_BOX(mainBox), statusRow);

    // Separator
    gtk_box_append(GTK_BOX(mainBox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // Buttons
    GtkWidget* btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btnRow, GTK_ALIGN_END);

    GtkWidget* cancelBtn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancelBtn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ud) {
                         auto* d = static_cast<CreateDatabaseDialogData*>(ud);
                         gtk_window_destroy(GTK_WINDOW(d->dialog));
                     }),
                     data);

    data->createButton = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(data->createButton, "suggested-action");

    g_signal_connect(
        data->createButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer ud) {
            auto* data = static_cast<CreateDatabaseDialogData*>(ud);

            const char* name = gtk_editable_get_text(GTK_EDITABLE(data->nameEntry));
            if (!name || strlen(name) == 0) {
                gtk_label_set_text(GTK_LABEL(data->statusLabel), "Please enter a database name");
                return;
            }

            gtk_widget_set_sensitive(data->createButton, FALSE);
            gtk_spinner_start(GTK_SPINNER(data->spinner));
            gtk_label_set_text(GTK_LABEL(data->statusLabel), "Creating...");

            CreateDatabaseOptions opts;
            opts.name = name;
            const char* comment = gtk_editable_get_text(GTK_EDITABLE(data->commentEntry));
            opts.comment = comment ? comment : "";

            auto getDropdownText = [](GtkWidget* dd) -> std::string {
                if (!dd)
                    return "";
                guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
                auto* model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(dd)));
                const char* s = gtk_string_list_get_string(model, idx);
                return s ? s : "";
            };

            DatabaseType type = data->db->getConnectionInfo().type;
            if (type == DatabaseType::POSTGRESQL) {
                opts.owner = getDropdownText(data->ownerDropdown);
                opts.templateDb = getDropdownText(data->templateDropdown);
                opts.encoding = getDropdownText(data->encodingDropdown);
                opts.tablespace = getDropdownText(data->tablespaceDropdown);
            } else {
                opts.charset = getDropdownText(data->charsetDropdown);
                opts.collation = getDropdownText(data->collationDropdown);
            }

            auto dbCopy = data->db;
            GtkWidget* dialogRef = data->dialog;
            g_object_ref(dialogRef);

            std::thread([=]() {
                auto result = dbCopy->createDatabaseWithOptions(opts);
                bool ok = result.first;
                std::string errMsg = result.second;

                struct CreateResult {
                    GtkWidget* dialogRef;
                    bool ok;
                    std::string error;
                    std::shared_ptr<DatabaseInterface> db;
                };

                auto* cr = new CreateResult{dialogRef, ok, errMsg, dbCopy};

                g_idle_add(
                    +[](gpointer p) -> gboolean {
                        auto* cr = static_cast<CreateResult*>(p);
                        auto* d = static_cast<CreateDatabaseDialogData*>(
                            g_object_get_data(G_OBJECT(cr->dialogRef), "data"));

                        if (cr->ok) {
                            if (auto* pgDb = dynamic_cast<PostgresDatabase*>(cr->db.get())) {
                                pgDb->refreshDatabaseNames();
                            } else if (auto* myDb = dynamic_cast<MySQLDatabase*>(cr->db.get())) {
                                myDb->refreshDatabaseNames();
                            }
                            gtk_window_destroy(GTK_WINDOW(cr->dialogRef));
                        } else if (d) {
                            gtk_label_set_text(GTK_LABEL(d->statusLabel),
                                               ("Failed: " + cr->error).c_str());
                            gtk_widget_set_sensitive(d->createButton, TRUE);
                            gtk_spinner_stop(GTK_SPINNER(d->spinner));
                        }

                        g_object_unref(cr->dialogRef);
                        delete cr;
                        return G_SOURCE_REMOVE;
                    },
                    cr);
            }).detach();
        }),
        data);

    gtk_box_append(GTK_BOX(btnRow), cancelBtn);
    gtk_box_append(GTK_BOX(btnRow), data->createButton);
    gtk_box_append(GTK_BOX(mainBox), btnRow);

    gtk_window_set_child(GTK_WINDOW(dialog), mainBox);

    g_object_set_data_full(G_OBJECT(dialog), "data", data, destroyCreateDbDialogData);
    // Clear singleton on destroy (fires for both close-request and gtk_window_destroy)
    g_signal_connect(
        dialog, "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer) { sActiveCreateDatabaseDialog = nullptr; }), nullptr);

    return dialog;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void showLinuxConnectionDialog(Application* app) {
    if (sActiveConnectionDialog) {
        gtk_window_present(GTK_WINDOW(sActiveConnectionDialog));
        return;
    }
    auto* data = new ConnectionDialogData();
    data->app = app;
    GtkWidget* dialog = buildConnectionDialog(data);
    sActiveConnectionDialog = dialog;
    gtk_window_present(GTK_WINDOW(dialog));
}

void showLinuxEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db,
                                   int connectionId) {
    if (sActiveConnectionDialog) {
        gtk_window_present(GTK_WINDOW(sActiveConnectionDialog));
        return;
    }
    auto* data = new ConnectionDialogData();
    data->app = app;
    data->editingDb = db;
    data->editingConnectionId = connectionId;
    GtkWidget* dialog = buildConnectionDialog(data);

    populateFieldsFromConnection(data, db);
    gtk_widget_set_sensitive(data->typeDropdown, FALSE);
    gtk_window_set_title(GTK_WINDOW(dialog), "Edit Connection");
    gtk_button_set_label(GTK_BUTTON(data->connectButton), "Update");

    sActiveConnectionDialog = dialog;
    gtk_window_present(GTK_WINDOW(dialog));
}

void showLinuxCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db) {
    if (sActiveCreateDatabaseDialog) {
        gtk_window_present(GTK_WINDOW(sActiveCreateDatabaseDialog));
        return;
    }
    auto* data = new CreateDatabaseDialogData();
    data->app = app;
    data->db = db;
    GtkWidget* dialog = buildCreateDatabaseDialog(data);
    sActiveCreateDatabaseDialog = dialog;
    gtk_window_present(GTK_WINDOW(dialog));
}

#endif // defined(__linux__)
