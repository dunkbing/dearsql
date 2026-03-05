#if defined(_WIN32)

#include "app_state.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "database/ssh_config_parser.hpp"
#include "database/ssl_config.hpp"
#include "platform/connection_dialog.hpp"
#include "platform/windows_platform.hpp"
#include "utils/file_dialog.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <atomic>
#include <commctrl.h>
#include <iostream>
#include <thread>
#include <windows.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

// ---------------------------------------------------------------------------
// control IDs
// ---------------------------------------------------------------------------
enum {
    IDC_NAME_EDIT = 1001,
    IDC_TYPE_COMBO,
    IDC_SQLITE_PATH_EDIT,
    IDC_SQLITE_BROWSE_BTN,
    IDC_HOST_EDIT,
    IDC_PORT_EDIT,
    IDC_DATABASE_EDIT,
    IDC_SSL_MODE_COMBO,
    IDC_SSL_CA_CERT_EDIT,
    IDC_SSL_CA_CERT_BROWSE,
    IDC_SSL_CA_CERT_LABEL,
    IDC_AUTH_PASSWORD_RADIO,
    IDC_AUTH_NONE_RADIO,
    IDC_USERNAME_EDIT,
    IDC_PASSWORD_EDIT,
    IDC_SHOW_ALL_DBS_CHECK,
    IDC_SSH_ENABLED_CHECK,
    IDC_SSH_HOST_EDIT,
    IDC_SSH_PORT_EDIT,
    IDC_SSH_USERNAME_EDIT,
    IDC_SSH_AUTH_PASSWORD_RADIO,
    IDC_SSH_AUTH_KEY_RADIO,
    IDC_SSH_PASSWORD_EDIT,
    IDC_SSH_KEY_EDIT,
    IDC_SSH_KEY_BROWSE,
    IDC_STATUS_LABEL,
    IDC_CONNECT_BTN,
    IDC_CANCEL_BTN,
    // labels (static text)
    IDC_LABEL_NAME,
    IDC_LABEL_TYPE,
    IDC_LABEL_PATH,
    IDC_LABEL_HOST,
    IDC_LABEL_PORT,
    IDC_LABEL_DATABASE,
    IDC_LABEL_SSL,
    IDC_LABEL_USERNAME,
    IDC_LABEL_PASSWORD,
    IDC_LABEL_SSH_HOST,
    IDC_LABEL_SSH_PORT,
    IDC_LABEL_SSH_USERNAME,
    IDC_LABEL_SSH_PASSWORD,
    IDC_LABEL_SSH_KEY,
};

// custom message for async connect result
#define WM_CONNECT_RESULT (WM_APP + 1)

// ---------------------------------------------------------------------------
// internal state
// ---------------------------------------------------------------------------
struct ConnectionDialogData {
    Application* app = nullptr;
    std::shared_ptr<DatabaseInterface> editingDb;
    int editingConnectionId = -1;
    std::atomic<bool> cancelled{false};

    HWND dialog = nullptr;
    HWND parentHwnd = nullptr;

    // cached SSH config
    std::vector<SSHHostEntry> sshConfigHosts;
    bool sshConfigLoaded = false;

    // current database type index
    int currentTypeIndex = 0;
};

struct AsyncConnectResult {
    HWND dialogRef;
    bool success;
    std::string error;
    std::shared_ptr<DatabaseInterface> db;
    DatabaseConnectionInfo info;
    Application* app;
    std::shared_ptr<DatabaseInterface> editingDb;
    int editingConnectionId;
    std::atomic<bool>* cancelledFlag;
};

struct CreateDatabaseDialogData {
    Application* app = nullptr;
    std::shared_ptr<DatabaseInterface> db;
    HWND dialog = nullptr;
    HWND parentHwnd = nullptr;
};

static HWND sActiveConnectionDialog = nullptr;
static HWND sActiveCreateDatabaseDialog = nullptr;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static HWND getParentHwnd(Application* app) {
    auto* platform = dynamic_cast<WindowsPlatform*>(app->getPlatform());
    return platform ? platform->getHWND() : nullptr;
}

static std::string getWindowText(HWND hwnd) {
    int len = GetWindowTextLengthA(hwnd);
    if (len <= 0)
        return "";
    std::string buf(len + 1, '\0');
    GetWindowTextA(hwnd, buf.data(), len + 1);
    buf.resize(len);
    return buf;
}

static void setStatus(HWND dialog, const std::string& text) {
    HWND label = GetDlgItem(dialog, IDC_STATUS_LABEL);
    if (label) {
        SetWindowTextA(label, text.c_str());
    }
}

static void setFormEnabled(HWND dialog, bool enabled) {
    BOOL e = enabled ? TRUE : FALSE;
    EnableWindow(GetDlgItem(dialog, IDC_NAME_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_TYPE_COMBO), e);
    EnableWindow(GetDlgItem(dialog, IDC_CONNECT_BTN), e);

    // SQLite
    EnableWindow(GetDlgItem(dialog, IDC_SQLITE_PATH_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SQLITE_BROWSE_BTN), e);

    // Server
    EnableWindow(GetDlgItem(dialog, IDC_HOST_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_PORT_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_DATABASE_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSL_MODE_COMBO), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSL_CA_CERT_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSL_CA_CERT_BROWSE), e);
    EnableWindow(GetDlgItem(dialog, IDC_AUTH_PASSWORD_RADIO), e);
    EnableWindow(GetDlgItem(dialog, IDC_AUTH_NONE_RADIO), e);
    EnableWindow(GetDlgItem(dialog, IDC_USERNAME_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_PASSWORD_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SHOW_ALL_DBS_CHECK), e);

    // SSH
    EnableWindow(GetDlgItem(dialog, IDC_SSH_ENABLED_CHECK), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_HOST_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_PORT_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_USERNAME_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_AUTH_PASSWORD_RADIO), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_AUTH_KEY_RADIO), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_PASSWORD_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_KEY_EDIT), e);
    EnableWindow(GetDlgItem(dialog, IDC_SSH_KEY_BROWSE), e);
}

static int defaultPort(DatabaseType type) {
    switch (type) {
    case DatabaseType::POSTGRESQL:
        return 5432;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        return 3306;
    case DatabaseType::MONGODB:
        return 27017;
    case DatabaseType::REDIS:
        return 6379;
    case DatabaseType::MSSQL:
        return 1433;
    default:
        return 0;
    }
}

// show/hide a control
static void showCtrl(HWND dialog, int id, bool show) {
    HWND ctrl = GetDlgItem(dialog, id);
    if (ctrl) {
        ShowWindow(ctrl, show ? SW_SHOW : SW_HIDE);
    }
}

static void updateFieldVisibility(HWND dialog, DatabaseType type) {
    bool isSQLite = (type == DatabaseType::SQLITE);
    bool isServer = !isSQLite;
    bool isRedis = (type == DatabaseType::REDIS);

    // SQLite fields
    showCtrl(dialog, IDC_LABEL_PATH, isSQLite);
    showCtrl(dialog, IDC_SQLITE_PATH_EDIT, isSQLite);
    showCtrl(dialog, IDC_SQLITE_BROWSE_BTN, isSQLite);

    // server fields
    showCtrl(dialog, IDC_LABEL_HOST, isServer);
    showCtrl(dialog, IDC_HOST_EDIT, isServer);
    showCtrl(dialog, IDC_LABEL_PORT, isServer);
    showCtrl(dialog, IDC_PORT_EDIT, isServer);
    showCtrl(dialog, IDC_LABEL_DATABASE, isServer && !isRedis);
    showCtrl(dialog, IDC_DATABASE_EDIT, isServer && !isRedis);
    showCtrl(dialog, IDC_LABEL_SSL, isServer);
    showCtrl(dialog, IDC_SSL_MODE_COMBO, isServer);

    // auth fields
    showCtrl(dialog, IDC_AUTH_PASSWORD_RADIO, isServer);
    showCtrl(dialog, IDC_AUTH_NONE_RADIO, isServer);
    showCtrl(dialog, IDC_LABEL_USERNAME, isServer);
    showCtrl(dialog, IDC_USERNAME_EDIT, isServer);
    showCtrl(dialog, IDC_LABEL_PASSWORD, isServer);
    showCtrl(dialog, IDC_PASSWORD_EDIT, isServer);

    showCtrl(dialog, IDC_SHOW_ALL_DBS_CHECK, isServer && !isRedis);

    // SSH fields
    showCtrl(dialog, IDC_SSH_ENABLED_CHECK, isServer);
    bool sshEnabled = isServer && (SendMessage(GetDlgItem(dialog, IDC_SSH_ENABLED_CHECK),
                                               BM_GETCHECK, 0, 0) == BST_CHECKED);
    showCtrl(dialog, IDC_LABEL_SSH_HOST, sshEnabled);
    showCtrl(dialog, IDC_SSH_HOST_EDIT, sshEnabled);
    showCtrl(dialog, IDC_LABEL_SSH_PORT, sshEnabled);
    showCtrl(dialog, IDC_SSH_PORT_EDIT, sshEnabled);
    showCtrl(dialog, IDC_LABEL_SSH_USERNAME, sshEnabled);
    showCtrl(dialog, IDC_SSH_USERNAME_EDIT, sshEnabled);
    showCtrl(dialog, IDC_SSH_AUTH_PASSWORD_RADIO, sshEnabled);
    showCtrl(dialog, IDC_SSH_AUTH_KEY_RADIO, sshEnabled);

    bool sshKeyAuth = sshEnabled && (SendMessage(GetDlgItem(dialog, IDC_SSH_AUTH_KEY_RADIO),
                                                 BM_GETCHECK, 0, 0) == BST_CHECKED);
    showCtrl(dialog, IDC_LABEL_SSH_PASSWORD, sshEnabled && !sshKeyAuth);
    showCtrl(dialog, IDC_SSH_PASSWORD_EDIT, sshEnabled && !sshKeyAuth);
    showCtrl(dialog, IDC_LABEL_SSH_KEY, sshEnabled && sshKeyAuth);
    showCtrl(dialog, IDC_SSH_KEY_EDIT, sshEnabled && sshKeyAuth);
    showCtrl(dialog, IDC_SSH_KEY_BROWSE, sshEnabled && sshKeyAuth);

    // SSL CA cert visibility
    if (isServer) {
        HWND sslCombo = GetDlgItem(dialog, IDC_SSL_MODE_COMBO);
        int sslIdx = static_cast<int>(SendMessage(sslCombo, CB_GETCURSEL, 0, 0));
        auto sslCfg = getSslConfig(type);
        bool needsCACert =
            (sslIdx >= 0 && sslIdx < sslCfg.count) && sslModeNeedsCACert(sslCfg.values[sslIdx]);
        showCtrl(dialog, IDC_SSL_CA_CERT_LABEL, needsCACert);
        showCtrl(dialog, IDC_SSL_CA_CERT_EDIT, needsCACert);
        showCtrl(dialog, IDC_SSL_CA_CERT_BROWSE, needsCACert);
    } else {
        showCtrl(dialog, IDC_SSL_CA_CERT_LABEL, false);
        showCtrl(dialog, IDC_SSL_CA_CERT_EDIT, false);
        showCtrl(dialog, IDC_SSL_CA_CERT_BROWSE, false);
    }

    // auth field visibility based on radio
    if (isServer) {
        bool authEnabled = SendMessage(GetDlgItem(dialog, IDC_AUTH_PASSWORD_RADIO), BM_GETCHECK, 0,
                                       0) == BST_CHECKED;
        EnableWindow(GetDlgItem(dialog, IDC_USERNAME_EDIT), authEnabled);
        EnableWindow(GetDlgItem(dialog, IDC_PASSWORD_EDIT), authEnabled);
    }
}

static void rebuildSslModes(HWND dialog, DatabaseType type) {
    HWND combo = GetDlgItem(dialog, IDC_SSL_MODE_COMBO);
    if (!combo)
        return;

    SendMessage(combo, CB_RESETCONTENT, 0, 0);
    auto cfg = getSslConfig(type);
    for (int i = 0; i < cfg.count; ++i) {
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg.labels[i]));
    }
    SendMessage(combo, CB_SETCURSEL, cfg.defaultIdx, 0);
}

// ---------------------------------------------------------------------------
// connection logic
// ---------------------------------------------------------------------------

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
    HWND dialog = data->dialog;
    std::string path = getWindowText(GetDlgItem(dialog, IDC_SQLITE_PATH_EDIT));
    if (path.empty()) {
        setStatus(dialog, "Please select a database file");
        return;
    }

    std::string name = getWindowText(GetDlgItem(dialog, IDC_NAME_EDIT));

    DatabaseConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.name = name;
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
        DestroyWindow(dialog);
    } else {
        setStatus(dialog, "Failed: " + error);
    }
}

static void connectServerAsync(ConnectionDialogData* data) {
    HWND dialog = data->dialog;

    std::string name = getWindowText(GetDlgItem(dialog, IDC_NAME_EDIT));
    auto type = static_cast<DatabaseType>(data->currentTypeIndex);

    std::string host = getWindowText(GetDlgItem(dialog, IDC_HOST_EDIT));
    std::string portStr = getWindowText(GetDlgItem(dialog, IDC_PORT_EDIT));
    int port = portStr.empty() ? defaultPort(type) : atoi(portStr.c_str());
    if (port <= 0)
        port = 1;
    if (port > 65535)
        port = 65535;

    std::string database = getWindowText(GetDlgItem(dialog, IDC_DATABASE_EDIT));
    bool authEnabled =
        SendMessage(GetDlgItem(dialog, IDC_AUTH_PASSWORD_RADIO), BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::string username = authEnabled ? getWindowText(GetDlgItem(dialog, IDC_USERNAME_EDIT)) : "";
    std::string password = authEnabled ? getWindowText(GetDlgItem(dialog, IDC_PASSWORD_EDIT)) : "";
    bool showAllDbs =
        SendMessage(GetDlgItem(dialog, IDC_SHOW_ALL_DBS_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;

    int sslModeIdx =
        static_cast<int>(SendMessage(GetDlgItem(dialog, IDC_SSL_MODE_COMBO), CB_GETCURSEL, 0, 0));
    std::string sslCACertPath = getWindowText(GetDlgItem(dialog, IDC_SSL_CA_CERT_EDIT));

    // SSH config
    bool sshEnabled =
        SendMessage(GetDlgItem(dialog, IDC_SSH_ENABLED_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    SSHConfig sshConfig;
    if (sshEnabled) {
        sshConfig.enabled = true;
        sshConfig.host = getWindowText(GetDlgItem(dialog, IDC_SSH_HOST_EDIT));
        std::string sshPortStr = getWindowText(GetDlgItem(dialog, IDC_SSH_PORT_EDIT));
        sshConfig.port = sshPortStr.empty() ? 22 : atoi(sshPortStr.c_str());
        if (sshConfig.port <= 0)
            sshConfig.port = 22;
        sshConfig.username = getWindowText(GetDlgItem(dialog, IDC_SSH_USERNAME_EDIT));

        bool useKey = SendMessage(GetDlgItem(dialog, IDC_SSH_AUTH_KEY_RADIO), BM_GETCHECK, 0, 0) ==
                      BST_CHECKED;
        if (useKey) {
            sshConfig.authMethod = SSHAuthMethod::PrivateKey;
            sshConfig.privateKeyPath = getWindowText(GetDlgItem(dialog, IDC_SSH_KEY_EDIT));
        } else {
            sshConfig.authMethod = SSHAuthMethod::Password;
            sshConfig.password = getWindowText(GetDlgItem(dialog, IDC_SSH_PASSWORD_EDIT));
        }
    }

    // validate
    if (authEnabled && username.empty() && type != DatabaseType::MONGODB &&
        type != DatabaseType::REDIS) {
        setStatus(dialog, "Please enter a username");
        return;
    }
    if (sshEnabled && sshConfig.host.empty()) {
        setStatus(dialog, "Please enter an SSH host");
        return;
    }
    if (sshEnabled && sshConfig.username.empty()) {
        setStatus(dialog, "Please enter an SSH username");
        return;
    }
    if (sshEnabled && sshConfig.authMethod == SSHAuthMethod::PrivateKey &&
        sshConfig.privateKeyPath.empty()) {
        setStatus(dialog, "Please select an SSH private key file");
        return;
    }

    // disable form
    setFormEnabled(dialog, false);
    setStatus(dialog, "Connecting...");

    Application* appPtr = data->app;
    auto editingDb = data->editingDb;
    int editConnId = data->editingConnectionId;
    HWND dialogRef = dialog;
    std::atomic<bool>* cancelledFlag = &data->cancelled;

    std::thread([=]() {
        DatabaseConnectionInfo info;
        info.type = type;
        info.name = name;
        info.host = host;
        info.port = port;
        info.showAllDatabases = showAllDbs;
        auto sslCfg = getSslConfig(type);
        info.sslmode = (sslModeIdx >= 0 && sslModeIdx < sslCfg.count) ? sslCfg.values[sslModeIdx]
                                                                      : SslMode::Disable;
        info.sslCACertPath = sslCACertPath;
        info.ssh = sshConfig;

        if (authEnabled) {
            info.username = username;
            info.password = password;
        }

        std::shared_ptr<DatabaseInterface> db;
        switch (type) {
        case DatabaseType::POSTGRESQL:
            info.database = database.empty() ? "postgres" : database;
            db = std::make_shared<PostgresDatabase>(info);
            break;
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB:
            info.database = database.empty() ? "mysql" : database;
            db = std::make_shared<MySQLDatabase>(info);
            break;
        case DatabaseType::MONGODB:
            info.database = database;
            db = std::make_shared<MongoDBDatabase>(info);
            break;
        case DatabaseType::REDIS:
            db = std::make_shared<RedisDatabase>(info);
            break;
        case DatabaseType::MSSQL:
            info.database = database.empty() ? "master" : database;
            db = std::make_shared<MSSQLDatabase>(info);
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

        PostMessage(dialogRef, WM_CONNECT_RESULT, 0, reinterpret_cast<LPARAM>(result));
    }).detach();
}

// ---------------------------------------------------------------------------
// window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK ConnectionDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<ConnectionDialogData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        data = static_cast<ConnectionDialogData*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        data->dialog = hwnd;

        HFONT hFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto makeCtrl = [&](const char* cls, const char* text, int id, DWORD style, int x, int y,
                            int w, int h) -> HWND {
            HWND ctrl = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                        GetModuleHandle(nullptr), nullptr);
            SendMessage(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            return ctrl;
        };

        constexpr int LX = 12;  // label x
        constexpr int FX = 100; // field x
        constexpr int FW = 340; // field width
        constexpr int LW = 82;  // label width
        constexpr int RH = 22;  // row height
        constexpr int RS = 28;  // row spacing
        int y = 12;

        // connection name
        makeCtrl("STATIC", "Name:", IDC_LABEL_NAME, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_NAME_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y, FW, RH);
        y += RS;

        // database type
        makeCtrl("STATIC", "Type:", IDC_LABEL_TYPE, SS_RIGHT, LX, y + 3, LW, RH);
        HWND typeCombo =
            makeCtrl("COMBOBOX", "", IDC_TYPE_COMBO, CBS_DROPDOWNLIST | WS_TABSTOP, FX, y, FW, 200);
        const char* types[] = {"SQLite", "PostgreSQL", "MySQL", "MariaDB",
                               "Redis",  "MongoDB",    "MSSQL"};
        for (const char* t : types) {
            SendMessageA(typeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
        }
        SendMessage(typeCombo, CB_SETCURSEL, 0, 0);
        y += RS;

        // SQLite path
        makeCtrl("STATIC", "Path:", IDC_LABEL_PATH, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_SQLITE_PATH_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y,
                 FW - 80, RH);
        makeCtrl("BUTTON", "Browse...", IDC_SQLITE_BROWSE_BTN, WS_TABSTOP | BS_PUSHBUTTON,
                 FX + FW - 74, y, 74, RH);
        y += RS;

        // host
        makeCtrl("STATIC", "Host:", IDC_LABEL_HOST, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "localhost", IDC_HOST_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y,
                 FW, RH);
        y += RS;

        // port
        makeCtrl("STATIC", "Port:", IDC_LABEL_PORT, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "5432", IDC_PORT_EDIT, WS_BORDER | WS_TABSTOP | ES_NUMBER, FX, y, 80, RH);
        y += RS;

        // database
        makeCtrl("STATIC", "Database:", IDC_LABEL_DATABASE, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_DATABASE_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y, FW,
                 RH);
        y += RS;

        // SSL mode
        makeCtrl("STATIC", "SSL:", IDC_LABEL_SSL, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("COMBOBOX", "", IDC_SSL_MODE_COMBO, CBS_DROPDOWNLIST | WS_TABSTOP, FX, y, FW, 200);
        y += RS;

        // SSL CA cert
        makeCtrl("STATIC", "CA cert:", IDC_SSL_CA_CERT_LABEL, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_SSL_CA_CERT_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y,
                 FW - 80, RH);
        makeCtrl("BUTTON", "Browse...", IDC_SSL_CA_CERT_BROWSE, WS_TABSTOP | BS_PUSHBUTTON,
                 FX + FW - 74, y, 74, RH);
        y += RS;

        // auth radio buttons
        makeCtrl("BUTTON", "Username && Password", IDC_AUTH_PASSWORD_RADIO,
                 BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, FX, y, 160, RH);
        makeCtrl("BUTTON", "No Auth", IDC_AUTH_NONE_RADIO, BS_AUTORADIOBUTTON | WS_TABSTOP,
                 FX + 170, y, 100, RH);
        SendMessage(GetDlgItem(hwnd, IDC_AUTH_PASSWORD_RADIO), BM_SETCHECK, BST_CHECKED, 0);
        y += RS;

        // username
        makeCtrl("STATIC", "Username:", IDC_LABEL_USERNAME, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_USERNAME_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y, FW,
                 RH);
        y += RS;

        // password
        makeCtrl("STATIC", "Password:", IDC_LABEL_PASSWORD, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_PASSWORD_EDIT,
                 WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, FX, y, FW, RH);
        y += RS;

        // show all databases
        makeCtrl("BUTTON", "Show all databases", IDC_SHOW_ALL_DBS_CHECK,
                 BS_AUTOCHECKBOX | WS_TABSTOP, FX, y, 180, RH);
        y += RS;

        // SSH tunnel section
        makeCtrl("BUTTON", "SSH Tunnel", IDC_SSH_ENABLED_CHECK, BS_AUTOCHECKBOX | WS_TABSTOP, FX, y,
                 120, RH);
        y += RS;

        // SSH host
        makeCtrl("STATIC", "SSH Host:", IDC_LABEL_SSH_HOST, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_SSH_HOST_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y, FW,
                 RH);
        y += RS;

        // SSH port
        makeCtrl("STATIC", "Port:", IDC_LABEL_SSH_PORT, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "22", IDC_SSH_PORT_EDIT, WS_BORDER | WS_TABSTOP | ES_NUMBER, FX, y, 80,
                 RH);
        y += RS;

        // SSH username
        makeCtrl("STATIC", "SSH User:", IDC_LABEL_SSH_USERNAME, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_SSH_USERNAME_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y,
                 FW, RH);
        y += RS;

        // SSH auth radio
        makeCtrl("BUTTON", "Password", IDC_SSH_AUTH_PASSWORD_RADIO,
                 BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, FX, y, 100, RH);
        makeCtrl("BUTTON", "Private Key", IDC_SSH_AUTH_KEY_RADIO, BS_AUTORADIOBUTTON | WS_TABSTOP,
                 FX + 110, y, 100, RH);
        SendMessage(GetDlgItem(hwnd, IDC_SSH_AUTH_PASSWORD_RADIO), BM_SETCHECK, BST_CHECKED, 0);
        y += RS;

        // SSH password
        makeCtrl("STATIC", "SSH Pass:", IDC_LABEL_SSH_PASSWORD, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_SSH_PASSWORD_EDIT,
                 WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, FX, y, FW, RH);
        y += RS;

        // SSH key path
        makeCtrl("STATIC", "Key file:", IDC_LABEL_SSH_KEY, SS_RIGHT, LX, y + 3, LW, RH);
        makeCtrl("EDIT", "", IDC_SSH_KEY_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, FX, y,
                 FW - 80, RH);
        makeCtrl("BUTTON", "Browse...", IDC_SSH_KEY_BROWSE, WS_TABSTOP | BS_PUSHBUTTON,
                 FX + FW - 74, y, 74, RH);
        y += RS + 4;

        // status label
        makeCtrl("STATIC", "", IDC_STATUS_LABEL, SS_LEFT, FX, y, FW, RH);
        y += RS;

        // buttons
        makeCtrl("BUTTON", "Cancel", IDC_CANCEL_BTN, WS_TABSTOP | BS_PUSHBUTTON, FX + FW - 170, y,
                 80, 28);
        makeCtrl("BUTTON", "Connect", IDC_CONNECT_BTN, WS_TABSTOP | BS_DEFPUSHBUTTON, FX + FW - 84,
                 y, 84, 28);

        // initial type is SQLite — rebuild SSL modes and field visibility
        rebuildSslModes(hwnd, DatabaseType::SQLITE);
        updateFieldVisibility(hwnd, DatabaseType::SQLITE);

        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int notif = HIWORD(wParam);

        if (id == IDC_TYPE_COMBO && notif == CBN_SELCHANGE) {
            HWND combo = GetDlgItem(hwnd, IDC_TYPE_COMBO);
            int sel = static_cast<int>(SendMessage(combo, CB_GETCURSEL, 0, 0));
            if (data) {
                data->currentTypeIndex = sel;
            }
            auto type = static_cast<DatabaseType>(sel);

            // update port
            SetWindowTextA(GetDlgItem(hwnd, IDC_PORT_EDIT),
                           std::to_string(defaultPort(type)).c_str());

            rebuildSslModes(hwnd, type);
            updateFieldVisibility(hwnd, type);
        } else if (id == IDC_SSL_MODE_COMBO && notif == CBN_SELCHANGE) {
            if (data) {
                updateFieldVisibility(hwnd, static_cast<DatabaseType>(data->currentTypeIndex));
            }
        } else if (id == IDC_AUTH_PASSWORD_RADIO || id == IDC_AUTH_NONE_RADIO) {
            if (data) {
                updateFieldVisibility(hwnd, static_cast<DatabaseType>(data->currentTypeIndex));
            }
        } else if (id == IDC_SSH_ENABLED_CHECK) {
            if (data) {
                updateFieldVisibility(hwnd, static_cast<DatabaseType>(data->currentTypeIndex));
            }
        } else if (id == IDC_SSH_AUTH_PASSWORD_RADIO || id == IDC_SSH_AUTH_KEY_RADIO) {
            if (data) {
                updateFieldVisibility(hwnd, static_cast<DatabaseType>(data->currentTypeIndex));
            }
        } else if (id == IDC_SQLITE_BROWSE_BTN) {
            OPENFILENAMEA ofn = {};
            char filePath[MAX_PATH] = "";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "SQLite Database\0*.db;*.sqlite;*.sqlite3\0All Files\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                SetWindowTextA(GetDlgItem(hwnd, IDC_SQLITE_PATH_EDIT), filePath);
            }
        } else if (id == IDC_SSL_CA_CERT_BROWSE) {
            OPENFILENAMEA ofn = {};
            char filePath[MAX_PATH] = "";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "Certificate Files\0*.pem;*.crt;*.cer\0All Files\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                SetWindowTextA(GetDlgItem(hwnd, IDC_SSL_CA_CERT_EDIT), filePath);
            }
        } else if (id == IDC_SSH_KEY_BROWSE) {
            OPENFILENAMEA ofn = {};
            char filePath[MAX_PATH] = "";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = "Private Key Files\0*.pem;*.key;*\0All Files\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                SetWindowTextA(GetDlgItem(hwnd, IDC_SSH_KEY_EDIT), filePath);
            }
        } else if (id == IDC_CONNECT_BTN) {
            if (!data)
                break;
            auto type = static_cast<DatabaseType>(data->currentTypeIndex);
            if (type == DatabaseType::SQLITE) {
                connectSQLite(data);
            } else {
                connectServerAsync(data);
            }
        } else if (id == IDC_CANCEL_BTN) {
            if (data) {
                data->cancelled.store(true);
            }
            DestroyWindow(hwnd);
        }
        break;
    }

    case WM_CONNECT_RESULT: {
        auto* result = reinterpret_cast<AsyncConnectResult*>(lParam);
        if (!result)
            break;

        if (result->cancelledFlag->load()) {
            if (result->success && result->db) {
                result->db->disconnect();
            }
        } else if (result->success) {
            handleConnectionSuccess(result);
            DestroyWindow(hwnd);
        } else {
            setStatus(hwnd, "Failed: " + result->error);
            setFormEnabled(hwnd, true);
            // keep type combo disabled during edit
            if (data && data->editingConnectionId != -1) {
                EnableWindow(GetDlgItem(hwnd, IDC_TYPE_COMBO), FALSE);
            }
        }
        delete result;
        return 0;
    }

    case WM_CLOSE:
        if (data) {
            data->cancelled.store(true);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        sActiveConnectionDialog = nullptr;
        if (data) {
            data->editingDb.reset();
            delete data;
        }
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        return 0;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// register window class (once)
// ---------------------------------------------------------------------------

static const char* kConnectionDialogClass = "DearSQL_ConnectionDialog";
static const char* kCreateDbDialogClass = "DearSQL_CreateDbDialog";

static void ensureWindowClassRegistered(const char* className, WNDPROC proc) {
    WNDCLASSEXA wc = {};
    if (GetClassInfoExA(GetModuleHandle(nullptr), className, &wc)) {
        return; // already registered
    }
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    RegisterClassExA(&wc);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

static void showConnectionDialogInternal(Application* app,
                                         std::shared_ptr<DatabaseInterface> editingDb) {
    if (sActiveConnectionDialog) {
        SetForegroundWindow(sActiveConnectionDialog);
        return;
    }

    ensureWindowClassRegistered(kConnectionDialogClass, ConnectionDialogProc);

    auto* data = new ConnectionDialogData();
    data->app = app;
    data->editingDb = editingDb;
    data->parentHwnd = getParentHwnd(app);

    if (editingDb) {
        data->editingConnectionId = editingDb->getConnectionId();
    }

    constexpr int dialogW = 480;
    constexpr int dialogH = 680;

    const char* title = editingDb ? "Edit Connection" : "New Connection";

    HWND hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, kConnectionDialogClass, title,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, dialogW, dialogH, data->parentHwnd, nullptr,
                                GetModuleHandle(nullptr), data);

    sActiveConnectionDialog = hwnd;

    if (editingDb) {
        // populate fields from existing connection
        const auto& info = editingDb->getConnectionInfo();
        SetWindowTextA(GetDlgItem(hwnd, IDC_NAME_EDIT), info.name.c_str());

        int typeIdx = static_cast<int>(info.type);
        SendMessage(GetDlgItem(hwnd, IDC_TYPE_COMBO), CB_SETCURSEL, typeIdx, 0);
        data->currentTypeIndex = typeIdx;
        EnableWindow(GetDlgItem(hwnd, IDC_TYPE_COMBO), FALSE);

        rebuildSslModes(hwnd, info.type);

        if (info.type == DatabaseType::SQLITE) {
            SetWindowTextA(GetDlgItem(hwnd, IDC_SQLITE_PATH_EDIT), info.path.c_str());
        } else {
            SetWindowTextA(GetDlgItem(hwnd, IDC_HOST_EDIT), info.host.c_str());
            SetWindowTextA(GetDlgItem(hwnd, IDC_PORT_EDIT), std::to_string(info.port).c_str());
            SetWindowTextA(GetDlgItem(hwnd, IDC_DATABASE_EDIT), info.database.c_str());
            SetWindowTextA(GetDlgItem(hwnd, IDC_USERNAME_EDIT), info.username.c_str());
            SetWindowTextA(GetDlgItem(hwnd, IDC_PASSWORD_EDIT), info.password.c_str());

            if (info.username.empty() && info.password.empty()) {
                SendMessage(GetDlgItem(hwnd, IDC_AUTH_NONE_RADIO), BM_SETCHECK, BST_CHECKED, 0);
                SendMessage(GetDlgItem(hwnd, IDC_AUTH_PASSWORD_RADIO), BM_SETCHECK, BST_UNCHECKED,
                            0);
            }

            if (info.showAllDatabases) {
                SendMessage(GetDlgItem(hwnd, IDC_SHOW_ALL_DBS_CHECK), BM_SETCHECK, BST_CHECKED, 0);
            }

            // SSL
            auto sslCfg = getSslConfig(info.type);
            for (int i = 0; i < sslCfg.count; ++i) {
                if (sslCfg.values[i] == info.sslmode) {
                    SendMessage(GetDlgItem(hwnd, IDC_SSL_MODE_COMBO), CB_SETCURSEL, i, 0);
                    break;
                }
            }
            SetWindowTextA(GetDlgItem(hwnd, IDC_SSL_CA_CERT_EDIT), info.sslCACertPath.c_str());

            // SSH
            if (info.ssh.enabled) {
                SendMessage(GetDlgItem(hwnd, IDC_SSH_ENABLED_CHECK), BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextA(GetDlgItem(hwnd, IDC_SSH_HOST_EDIT), info.ssh.host.c_str());
                SetWindowTextA(GetDlgItem(hwnd, IDC_SSH_PORT_EDIT),
                               std::to_string(info.ssh.port).c_str());
                SetWindowTextA(GetDlgItem(hwnd, IDC_SSH_USERNAME_EDIT), info.ssh.username.c_str());
                if (info.ssh.authMethod == SSHAuthMethod::PrivateKey) {
                    SendMessage(GetDlgItem(hwnd, IDC_SSH_AUTH_KEY_RADIO), BM_SETCHECK, BST_CHECKED,
                                0);
                    SendMessage(GetDlgItem(hwnd, IDC_SSH_AUTH_PASSWORD_RADIO), BM_SETCHECK,
                                BST_UNCHECKED, 0);
                    SetWindowTextA(GetDlgItem(hwnd, IDC_SSH_KEY_EDIT),
                                   info.ssh.privateKeyPath.c_str());
                } else {
                    SetWindowTextA(GetDlgItem(hwnd, IDC_SSH_PASSWORD_EDIT),
                                   info.ssh.password.c_str());
                }
            }
        }

        updateFieldVisibility(hwnd, info.type);

        // change connect button text
        SetWindowTextA(GetDlgItem(hwnd, IDC_CONNECT_BTN), "Update");
    }
}

void showConnectionDialog(Application* app) {
    showConnectionDialogInternal(app, nullptr);
}

void showEditConnectionDialog(Application* app, std::shared_ptr<DatabaseInterface> db) {
    showConnectionDialogInternal(app, db);
}

// ---------------------------------------------------------------------------
// create database dialog
// ---------------------------------------------------------------------------

static LRESULT CALLBACK CreateDatabaseDialogProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam) {
    auto* data = reinterpret_cast<CreateDatabaseDialogData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        data = static_cast<CreateDatabaseDialogData*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        data->dialog = hwnd;

        HFONT hFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto makeCtrl = [&](const char* cls, const char* text, int id, DWORD style, int x, int y,
                            int w, int h) -> HWND {
            HWND ctrl = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                        GetModuleHandle(nullptr), nullptr);
            SendMessage(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            return ctrl;
        };

        int y = 16;
        makeCtrl("STATIC", "Database name:", 2000, SS_LEFT, 16, y + 3, 100, 20);
        makeCtrl("EDIT", "", 2001, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 120, y, 220, 22);
        y += 36;

        makeCtrl("STATIC", "", 2010, SS_LEFT, 16, y, 320, 20); // status
        y += 28;

        makeCtrl("BUTTON", "Cancel", 2002, WS_TABSTOP | BS_PUSHBUTTON, 170, y, 80, 28);
        makeCtrl("BUTTON", "Create", 2003, WS_TABSTOP | BS_DEFPUSHBUTTON, 256, y, 84, 28);

        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 2002) { // Cancel
            DestroyWindow(hwnd);
        } else if (id == 2003) { // Create
            if (!data || !data->db)
                break;

            char buf[256] = "";
            GetWindowTextA(GetDlgItem(hwnd, 2001), buf, sizeof(buf));
            std::string dbName = buf;

            if (dbName.empty()) {
                SetWindowTextA(GetDlgItem(hwnd, 2010), "Please enter a database name");
                break;
            }

            SetWindowTextA(GetDlgItem(hwnd, 2010), "Creating...");
            EnableWindow(GetDlgItem(hwnd, 2003), FALSE);

            auto [success, error] = data->db->createDatabase(dbName);
            if (success) {
                DestroyWindow(hwnd);
            } else {
                SetWindowTextA(GetDlgItem(hwnd, 2010), ("Failed: " + error).c_str());
                EnableWindow(GetDlgItem(hwnd, 2003), TRUE);
            }
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        sActiveCreateDatabaseDialog = nullptr;
        if (data) {
            data->db.reset();
            delete data;
        }
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        return 0;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void showCreateDatabaseDialog(Application* app, std::shared_ptr<DatabaseInterface> db) {
    if (sActiveCreateDatabaseDialog) {
        SetForegroundWindow(sActiveCreateDatabaseDialog);
        return;
    }

    ensureWindowClassRegistered(kCreateDbDialogClass, CreateDatabaseDialogProc);

    auto* data = new CreateDatabaseDialogData();
    data->app = app;
    data->db = db;
    data->parentHwnd = getParentHwnd(app);

    HWND hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, kCreateDbDialogClass, "Create New Database",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, 370, 160, data->parentHwnd, nullptr,
                                GetModuleHandle(nullptr), data);

    sActiveCreateDatabaseDialog = hwnd;
}

#endif
