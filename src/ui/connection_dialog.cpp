#include "ui/connection_dialog.hpp"
#include "app_state.hpp"
#include "application.hpp"
#include "database/connection_url.hpp"
#include "database/sqlite.hpp"
#include "database/ssl_config.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/file_dialog.hpp"
#include "utils/spinner.hpp"
#include "utils/texture_manager.hpp"
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <format>

namespace {
    constexpr const char* kPopupId = "###connection_dialog";
    // shown in place of the real password in the url field
    constexpr const char* kPasswordMask = "****";
    constexpr float kDialogWidth = 500.0f;
    constexpr float kLabelColumnW = 110.0f;
    constexpr float kButtonW = 90.0f;

    // popup index == DatabaseType enum value
    constexpr const char* kTypeLabels[] = {"SQLite",   "PostgreSQL", "MySQL", "MariaDB",
                                           "Redis",    "MongoDB",    "MSSQL", "Oracle",
                                           "Redshift", "Cassandra"};
    constexpr int kTypeCount = sizeof(kTypeLabels) / sizeof(kTypeLabels[0]);

    void copyToBuf(char* dst, size_t dstSize, const std::string& src) {
        std::strncpy(dst, src.c_str(), dstSize - 1);
        dst[dstSize - 1] = '\0';
    }

    int parsePort(const char* buf, int fallback) {
        int port = std::atoi(buf);
        if (port <= 0 || port > 65535)
            return fallback;
        return port;
    }

    void fieldLabel(const char* text) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, "%s", text);
        ImGui::SameLine(kLabelColumnW);
    }

    bool shouldShowCACertField(DatabaseType type, SslMode mode) {
        if (sslModeNeedsCACert(mode))
            return true;
        return (type == DatabaseType::MYSQL || type == DatabaseType::MARIADB) &&
               mode == SslMode::Require;
    }

    bool sslModesMatchForType(DatabaseType type, SslMode lhs, SslMode rhs) {
        if (lhs == rhs)
            return true;
        const bool mysqlFamily = type == DatabaseType::MYSQL || type == DatabaseType::MARIADB;
        if (!mysqlFamily)
            return false;
        const bool lhsVerifiesIdentity =
            lhs == SslMode::VerifyFull || lhs == SslMode::VerifyIdentity;
        const bool rhsVerifiesIdentity =
            rhs == SslMode::VerifyFull || rhs == SslMode::VerifyIdentity;
        return lhsVerifiesIdentity && rhsVerifiesIdentity;
    }

    int sslIndexForMode(DatabaseType type, SslMode mode) {
        auto cfg = getSslConfig(type);
        for (int i = 0; i < cfg.count; i++) {
            if (sslModesMatchForType(type, mode, cfg.values[i]))
                return i;
        }
        return cfg.defaultIdx;
    }
} // namespace

ConnectionDialog& ConnectionDialog::instance() {
    static ConnectionDialog dialog;
    return dialog;
}

void ConnectionDialog::show(Application* app) {
    if (open_)
        return;
    app_ = app;
    editingDb_.reset();
    editingConnectionId_ = -1;
    resetForm();
    open_ = true;
    pendingOpen_ = true;
}

void ConnectionDialog::showEdit(Application* app, std::shared_ptr<DatabaseInterface> db) {
    if (open_ || !db)
        return;
    app_ = app;
    editingDb_ = db;
    editingConnectionId_ = db->getConnectionId();
    resetForm();
    populateForm(db->getConnectionInfo());
    open_ = true;
    pendingOpen_ = true;
}

void ConnectionDialog::resetForm() {
    typeIdx_ = 0;
    connectByIdx_ = 0;
    copyToBuf(nameBuf_, sizeof(nameBuf_), "Untitled connection");
    urlBuf_[0] = '\0';
    urlError_.clear();
    sqlitePathBuf_[0] = '\0';
    copyToBuf(hostBuf_, sizeof(hostBuf_), "localhost");
    copyToBuf(portBuf_, sizeof(portBuf_), "5432");
    databaseBuf_[0] = '\0';
    sslModeIdx_ = getSslConfig(DatabaseType::SQLITE).defaultIdx;
    sslCACertPathBuf_[0] = '\0';
    authIdx_ = 0;
    usernameBuf_[0] = '\0';
    passwordBuf_[0] = '\0';
    showAllDbs_ = false;
    sshEnabled_ = false;
    sshHostBuf_[0] = '\0';
    copyToBuf(sshPortBuf_, sizeof(sshPortBuf_), "22");
    sshUsernameBuf_[0] = '\0';
    sshAuthIdx_ = 0;
    sshPasswordBuf_[0] = '\0';
    sshKeyPathBuf_[0] = '\0';
    statusText_.clear();
    statusIsError_ = false;
    closeRequested_ = false;
    cancelled_ = false;
    installingOracle_ = false;
}

void ConnectionDialog::applyTypeDefaults(DatabaseType type) {
    switch (type) {
    case DatabaseType::SQLITE:
        break;
    case DatabaseType::POSTGRESQL:
        copyToBuf(portBuf_, sizeof(portBuf_), "5432");
        authIdx_ = 0;
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        copyToBuf(portBuf_, sizeof(portBuf_), "3306");
        authIdx_ = 0;
        break;
    case DatabaseType::MONGODB:
        copyToBuf(portBuf_, sizeof(portBuf_), "27017");
        authIdx_ = 1;
        break;
    case DatabaseType::REDIS:
        copyToBuf(portBuf_, sizeof(portBuf_), "6379");
        authIdx_ = 1;
        break;
    case DatabaseType::MSSQL:
        copyToBuf(portBuf_, sizeof(portBuf_), "1433");
        authIdx_ = 0;
        break;
    case DatabaseType::ORACLE:
        copyToBuf(portBuf_, sizeof(portBuf_), "1521");
        authIdx_ = 0;
        break;
    case DatabaseType::REDSHIFT:
        copyToBuf(portBuf_, sizeof(portBuf_), "5439");
        authIdx_ = 0;
        break;
    case DatabaseType::CASSANDRA:
        copyToBuf(portBuf_, sizeof(portBuf_), "9042");
        authIdx_ = 1;
        break;
    }
    sslModeIdx_ = getSslConfig(type).defaultIdx;
    setStatus("", false);
}

void ConnectionDialog::populateForm(const DatabaseConnectionInfo& info) {
    typeIdx_ = static_cast<int>(info.type);
    copyToBuf(nameBuf_, sizeof(nameBuf_), info.name);

    if (info.type == DatabaseType::SQLITE) {
        copyToBuf(sqlitePathBuf_, sizeof(sqlitePathBuf_), info.path);
        return;
    }

    copyToBuf(hostBuf_, sizeof(hostBuf_), info.host);
    if (info.port > 0)
        copyToBuf(portBuf_, sizeof(portBuf_), std::to_string(info.port));
    copyToBuf(databaseBuf_, sizeof(databaseBuf_), info.database);
    showAllDbs_ = info.showAllDatabases;

    if (!info.username.empty() || !info.password.empty()) {
        authIdx_ = 0;
        copyToBuf(usernameBuf_, sizeof(usernameBuf_), info.username);
        copyToBuf(passwordBuf_, sizeof(passwordBuf_), info.password);
    } else {
        authIdx_ = 1;
        usernameBuf_[0] = '\0';
        passwordBuf_[0] = '\0';
    }

    sslModeIdx_ = sslIndexForMode(info.type, info.sslmode);
    copyToBuf(sslCACertPathBuf_, sizeof(sslCACertPathBuf_), info.sslCACertPath);

    if (info.ssh.enabled) {
        sshEnabled_ = true;
        copyToBuf(sshHostBuf_, sizeof(sshHostBuf_), info.ssh.host);
        copyToBuf(sshPortBuf_, sizeof(sshPortBuf_), std::to_string(info.ssh.port));
        copyToBuf(sshUsernameBuf_, sizeof(sshUsernameBuf_), info.ssh.username);
        if (info.ssh.authMethod == SSHAuthMethod::PrivateKey) {
            sshAuthIdx_ = 1;
            copyToBuf(sshKeyPathBuf_, sizeof(sshKeyPathBuf_), info.ssh.privateKeyPath);
        } else {
            sshAuthIdx_ = 0;
        }
        // password auth password or key passphrase, depending on method
        copyToBuf(sshPasswordBuf_, sizeof(sshPasswordBuf_), info.ssh.password);
    }

    rebuildUrlFromForm();
}

DatabaseConnectionInfo ConnectionDialog::snapshotForm() const {
    DatabaseConnectionInfo info;
    info.type = selectedType();
    info.name = nameBuf_;
    if (info.type == DatabaseType::SQLITE) {
        info.path = sqlitePathBuf_;
        return info;
    }
    info.host = hostBuf_;
    info.port = std::atoi(portBuf_);
    info.database = databaseBuf_;
    if (authIdx_ == 0) {
        info.username = usernameBuf_;
        info.password = passwordBuf_;
    }
    auto cfg = getSslConfig(info.type);
    if (sslModeIdx_ >= 0 && sslModeIdx_ < cfg.count)
        info.sslmode = cfg.values[sslModeIdx_];
    info.sslCACertPath = sslCACertPathBuf_;
    info.showAllDatabases = showAllDbs_;

    info.ssh.enabled = sshEnabled_;
    if (sshEnabled_) {
        info.ssh.host = sshHostBuf_;
        info.ssh.port = parsePort(sshPortBuf_, 22);
        info.ssh.username = sshUsernameBuf_;
        info.ssh.authMethod =
            (sshAuthIdx_ == 0) ? SSHAuthMethod::Password : SSHAuthMethod::PrivateKey;
        info.ssh.password = sshPasswordBuf_; // password or key passphrase
        if (info.ssh.authMethod == SSHAuthMethod::PrivateKey)
            info.ssh.privateKeyPath = sshKeyPathBuf_;
    }
    return info;
}

void ConnectionDialog::rebuildUrlFromForm() {
    DatabaseConnectionInfo info = snapshotForm();
    // never show the real password in the url field
    if (!info.password.empty())
        info.password = kPasswordMask;
    copyToBuf(urlBuf_, sizeof(urlBuf_), buildConnectionUrl(info));
    urlError_.clear();
}

void ConnectionDialog::applyUrlToForm() {
    if (urlBuf_[0] == '\0') {
        urlError_.clear();
        return;
    }
    auto result = parseConnectionUrl(urlBuf_);
    if (!result.ok) {
        urlError_ = "Error: " + result.error;
        return;
    }
    urlError_.clear();

    const auto& info = result.info;
    typeIdx_ = static_cast<int>(info.type);
    applyTypeDefaults(info.type);

    if (info.type == DatabaseType::SQLITE) {
        copyToBuf(sqlitePathBuf_, sizeof(sqlitePathBuf_), info.path);
        return;
    }

    copyToBuf(hostBuf_, sizeof(hostBuf_), info.host);
    if (info.port > 0)
        copyToBuf(portBuf_, sizeof(portBuf_), std::to_string(info.port));
    copyToBuf(databaseBuf_, sizeof(databaseBuf_), info.database);

    // the mask placeholder means "unchanged" — keep the form's password
    const std::string password =
        (info.password == kPasswordMask) ? std::string(passwordBuf_) : info.password;

    if (!info.username.empty() || !password.empty()) {
        authIdx_ = 0;
        copyToBuf(usernameBuf_, sizeof(usernameBuf_), info.username);
        copyToBuf(passwordBuf_, sizeof(passwordBuf_), password);
    } else {
        authIdx_ = 1;
        usernameBuf_[0] = '\0';
        passwordBuf_[0] = '\0';
    }

    sslModeIdx_ = sslIndexForMode(info.type, info.sslmode);
    copyToBuf(sslCACertPathBuf_, sizeof(sslCACertPathBuf_), info.sslCACertPath);
}

void ConnectionDialog::setStatus(const std::string& text, bool isError) {
    statusText_ = text;
    statusIsError_ = isError;
}

// MARK: - Connect

void ConnectionDialog::startConnect() {
    if (nameBuf_[0] == '\0') {
        setStatus("Please enter a connection name", true);
        return;
    }

    if (editingConnectionId_ == -1 && app_ && !app_->canAddConnection()) {
        setStatus("Connection limit reached (free tier: 3). Activate a license to add more.", true);
        return;
    }

    DatabaseType type = selectedType();

    if (type == DatabaseType::SQLITE) {
        connectSQLite();
        return;
    }

    // auth/ssh validation before kicking off anything async
    bool authEnabled = (authIdx_ == 0);
    if (authEnabled && usernameBuf_[0] == '\0' && type != DatabaseType::MONGODB &&
        type != DatabaseType::REDIS) {
        setStatus("Please enter a username", true);
        return;
    }
    if (sshEnabled_) {
        if (sshHostBuf_[0] == '\0') {
            setStatus("Please enter an SSH host", true);
            return;
        }
        if (sshUsernameBuf_[0] == '\0') {
            setStatus("Please enter an SSH username", true);
            return;
        }
        if (sshAuthIdx_ == 1 && sshKeyPathBuf_[0] == '\0') {
            setStatus("Please select an SSH private key file", true);
            return;
        }
    }

    if (type == DatabaseType::ORACLE && !OracleClientInstaller::isInstalled() &&
        !oracleInstaller_.isRunning()) {
        oracleInstaller_.startInstall();
        installingOracle_ = true;
        setStatus("Installing Oracle Instant Client...", false);
        return;
    }

    startServerConnect();
}

void ConnectionDialog::connectSQLite() {
    if (sqlitePathBuf_[0] == '\0') {
        setStatus("Please select a database file", true);
        return;
    }

    DatabaseConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.name = nameBuf_;
    info.path = sqlitePathBuf_;

    auto db = std::make_shared<SQLiteDatabase>(info);
    auto [success, error] = db->connect();
    if (success) {
        handleSuccess(db, info);
        closeRequested_ = true;
    } else {
        setStatus("Failed: " + error, true);
    }
}

void ConnectionDialog::startServerConnect() {
    DatabaseConnectionInfo info = snapshotForm();
    info.port = parsePort(portBuf_, 1);

    switch (info.type) {
    case DatabaseType::POSTGRESQL:
        if (info.database.empty())
            info.database = "postgres";
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        // empty database is fine: connects without a default schema (#19)
        break;
    case DatabaseType::MSSQL:
        if (info.database.empty())
            info.database = "master";
        break;
    case DatabaseType::REDSHIFT:
        if (info.database.empty())
            info.database = "dev";
        break;
    default:
        break;
    }

    cancelled_ = false;
    setStatus("Connecting...", false);

    connectOp_.start([info]() {
        ConnectResult result;
        result.info = info;
        result.db = DatabaseFactory::createDatabase(info);
        if (!result.db) {
            result.error = "Failed to create database connection";
            return result;
        }
        auto [success, error] = result.db->connect();
        result.success = success;
        result.error = error;
        return result;
    });
}

void ConnectionDialog::pollConnect() {
    connectOp_.check([this](ConnectResult result) {
        if (cancelled_) {
            // dialog was dismissed mid-connect — discard
            if (result.success && result.db)
                result.db->disconnect();
            return;
        }
        if (result.success) {
            handleSuccess(result.db, result.info);
            closeRequested_ = true;
        } else {
            setStatus("Failed: " + result.error, true);
        }
    });
}

void ConnectionDialog::pollOracleInstall() {
    if (!installingOracle_)
        return;

    oracleInstaller_.checkStatus();
    if (oracleInstaller_.isRunning()) {
        setStatus(oracleInstaller_.getStatusMessage(), false);
        return;
    }

    installingOracle_ = false;
    if (oracleInstaller_.getStatus() == OracleClientInstaller::Status::Done) {
        startServerConnect();
    } else {
        setStatus(oracleInstaller_.getError(), true);
    }
}

void ConnectionDialog::handleSuccess(const std::shared_ptr<DatabaseInterface>& db,
                                     const DatabaseConnectionInfo& info) {
    if (!app_)
        return;

    if (editingConnectionId_ != -1 && editingDb_) {
        SavedConnection conn;
        conn.id = editingConnectionId_;
        conn.connectionInfo = info;
        conn.workspaceId = app_->getCurrentWorkspaceId();
        app_->getAppState()->updateConnection(conn);

        db->setConnectionId(editingConnectionId_);
        auto& dbs = app_->getDatabases();
        for (size_t i = 0; i < dbs.size(); i++) {
            if (dbs[i] == editingDb_) {
                dbs[i]->disconnect();
                dbs[i] = db;
                break;
            }
        }
    } else {
        SavedConnection conn;
        conn.connectionInfo = info;
        conn.workspaceId = app_->getCurrentWorkspaceId();
        int newId = app_->saveConnection(conn);
        if (newId > 0)
            db->setConnectionId(newId);
        app_->addDatabase(db);
    }
}

void ConnectionDialog::finishClose(bool cancelled) {
    if (cancelled) {
        cancelled_ = true;
        if (installingOracle_ || oracleInstaller_.isRunning())
            oracleInstaller_.cancel();
    }
    installingOracle_ = false;
    open_ = false;
    pendingOpen_ = false;
    closeRequested_ = false;
    editingDb_.reset();
}

// MARK: - Render

void ConnectionDialog::render() {
    // keep polling even after the dialog closed so a cancelled connect's
    // result gets discarded instead of leaking a live connection
    pollConnect();

    if (!open_)
        return;

    pollOracleInstall();

    const bool editing = (editingConnectionId_ != -1);
    const std::string title =
        std::string(editing ? "Edit Connection" : "Connect to Database") + kPopupId;

    if (pendingOpen_) {
        ImGui::OpenPopup(title.c_str());
        pendingOpen_ = false;
    }

    // pin the top edge ~5% below the viewport top — the dialog grows
    // downward when a taller db type is picked (or ssh tunnel is expanded),
    // so anchoring the top high keeps the whole form on screen
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 pos(viewport->GetCenter().x, viewport->Pos.y + viewport->Size.y * 0.05f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(kDialogWidth, 0.0f), ImVec2(kDialogWidth, FLT_MAX));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, Application::getInstance().getCurrentColors().overlay1);

    bool stayOpen = true;
    if (!ImGui::BeginPopupModal(title.c_str(), &stayOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        // closed externally (e.g. escape)
        finishClose(true);
        return;
    }

    if (closeRequested_ || !stayOpen) {
        finishClose(!closeRequested_);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));

    const bool busy = isBusy();
    bool formChanged = false;

    ImGui::BeginDisabled(busy);

    fieldLabel("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##conn_name", "Connection name", nameBuf_, sizeof(nameBuf_));

    renderTypeRow();

    if (selectedType() != DatabaseType::SQLITE) {
        fieldLabel("Connect by");
        ImGui::RadioButton("Host##connect_by", &connectByIdx_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("URL##connect_by", &connectByIdx_, 1);

        renderUrlRow();
    }

    ImGui::Separator();

    if (selectedType() == DatabaseType::SQLITE) {
        renderSqliteFields(formChanged);
    } else {
        renderServerFields(formChanged);
        renderSshFields(formChanged);
    }

    ImGui::EndDisabled();

    if (formChanged)
        rebuildUrlFromForm();

    ImGui::Separator();
    renderStatusRow();

    bool connectClicked = renderButtons();

    ImGui::PopStyleVar();

    if (connectClicked && !busy)
        startConnect();

    ImGui::EndPopup();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void ConnectionDialog::renderTypeRow() {
    fieldLabel("Type");

    ImGui::BeginDisabled(editingConnectionId_ != -1);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("##conn_type", &typeIdx_, kTypeLabels, kTypeCount)) {
        applyTypeDefaults(selectedType());
        rebuildUrlFromForm();
    }
    ImGui::EndDisabled();

    ImTextureID icon = TextureManager::instance().getIcon(selectedType());
    if (icon != ImTextureID{}) {
        constexpr float kIconSize = 18.0f;
        // centre the icon against the combo's frame height
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - kIconSize) * 0.5f);
        ImGui::Image(icon, ImVec2(kIconSize, kIconSize));
    }
}

void ConnectionDialog::renderUrlRow() {
    fieldLabel("URL");
    ImGui::BeginDisabled(connectByIdx_ != 1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##conn_url", "postgresql://user:password@host:5432/dbname",
                                 urlBuf_, sizeof(urlBuf_))) {
        applyUrlToForm();
    }
    ImGui::EndDisabled();
    if (!urlError_.empty()) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::SetCursorPosX(kLabelColumnW);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("%s", urlError_.c_str());
        ImGui::PopStyleColor();
    }
}

void ConnectionDialog::renderSqliteFields(bool& formChanged) {
    fieldLabel("File");
    float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(-(browseW + Theme::Spacing::M));
    formChanged |= ImGui::InputTextWithHint("##sqlite_path", "Database file path", sqlitePathBuf_,
                                            sizeof(sqlitePathBuf_));
    ImGui::SameLine();
    if (ImGui::Button("Browse...##sqlite")) {
        auto db = FileDialog::openSQLiteFile();
        if (auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(db)) {
            copyToBuf(sqlitePathBuf_, sizeof(sqlitePathBuf_), sqliteDb->getPath());
            if (nameBuf_[0] == '\0' || std::strcmp(nameBuf_, "Untitled connection") == 0) {
                copyToBuf(nameBuf_, sizeof(nameBuf_), sqliteDb->getConnectionInfo().name);
            }
            formChanged = true;
        }
    }
}

void ConnectionDialog::renderServerFields(bool& formChanged) {
    DatabaseType type = selectedType();

    // url mode: the url field drives everything it can express, so lock the
    // form fields it maps to (ssh + show-all stay editable — not in the url)
    ImGui::BeginDisabled(connectByIdx_ == 1);

    // host + port on one row
    fieldLabel("Host");
    constexpr float kPortW = 70.0f;
    float portLabelW = ImGui::CalcTextSize("Port").x;
    ImGui::SetNextItemWidth(-(kPortW + portLabelW + Theme::Spacing::M * 2));
    formChanged |= ImGui::InputTextWithHint("##conn_host", "localhost", hostBuf_, sizeof(hostBuf_));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kPortW);
    formChanged |= ImGui::InputText("##conn_port", portBuf_, sizeof(portBuf_),
                                    ImGuiInputTextFlags_CharsDecimal);

    if (type != DatabaseType::REDIS) {
        fieldLabel("Database");
        ImGui::SetNextItemWidth(-FLT_MIN);
        formChanged |= ImGui::InputTextWithHint("##conn_database", "(optional)", databaseBuf_,
                                                sizeof(databaseBuf_));
        if (type == DatabaseType::POSTGRESQL) {
            ImGui::SetItemTooltip("Leave empty to use the default 'postgres' database");
        } else if (type == DatabaseType::REDSHIFT) {
            ImGui::SetItemTooltip("Leave empty to use the default 'dev' database");
        }
    }

    // ssl mode (per-backend options)
    auto sslCfg = getSslConfig(type);
    if (sslModeIdx_ < 0 || sslModeIdx_ >= sslCfg.count)
        sslModeIdx_ = sslCfg.defaultIdx;
    fieldLabel("SSL Mode");
    ImGui::SetNextItemWidth(180.0f);
    formChanged |= ImGui::Combo("##conn_sslmode", &sslModeIdx_, sslCfg.labels, sslCfg.count);

    if (shouldShowCACertField(type, sslCfg.values[sslModeIdx_])) {
        const bool isOracleWallet = (type == DatabaseType::ORACLE);
        fieldLabel(isOracleWallet ? "Wallet" : "CA Cert");
        float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(-(browseW + Theme::Spacing::M));
        formChanged |= ImGui::InputTextWithHint(
            "##conn_cacert", isOracleWallet ? "/path/to/wallet" : "/path/to/ca-cert.pem",
            sslCACertPathBuf_, sizeof(sslCACertPathBuf_));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##cacert")) {
            // oracle wallets are directories; other backends take a cert file
            std::string path = isOracleWallet ? FileDialog::pickFolder() : FileDialog::openFile();
            if (!path.empty()) {
                copyToBuf(sslCACertPathBuf_, sizeof(sslCACertPathBuf_), path);
                formChanged = true;
            }
        }
    }

    fieldLabel("Auth");
    formChanged |= ImGui::RadioButton("Username & Password##auth", &authIdx_, 0);
    ImGui::SameLine();
    formChanged |= ImGui::RadioButton("None##auth", &authIdx_, 1);

    if (authIdx_ == 0) {
        fieldLabel("Username");
        float passLabelW = ImGui::CalcTextSize("Password").x;
        float fieldW =
            (ImGui::GetContentRegionAvail().x - passLabelW - Theme::Spacing::M * 2) * 0.5f;
        ImGui::SetNextItemWidth(fieldW);
        formChanged |= ImGui::InputTextWithHint("##conn_username", "Username", usernameBuf_,
                                                sizeof(usernameBuf_));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Password");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        formChanged |= ImGui::InputTextWithHint("##conn_password", "Password", passwordBuf_,
                                                sizeof(passwordBuf_), ImGuiInputTextFlags_Password);
    }

    ImGui::EndDisabled();

    if (type != DatabaseType::REDIS) {
        ImGui::SetCursorPosX(kLabelColumnW);
        ImGui::Checkbox("Show all databases##conn", &showAllDbs_);
    }
}

void ConnectionDialog::renderSshFields(bool& formChanged) {
    ImGui::Separator();

    ImGui::SetCursorPosX(kLabelColumnW);
    ImGui::Checkbox("Connect via SSH tunnel##conn", &sshEnabled_);
    if (!sshEnabled_)
        return;

    fieldLabel("SSH Host");
    constexpr float kPortW = 70.0f;
    float portLabelW = ImGui::CalcTextSize("Port").x;
    ImGui::SetNextItemWidth(-(kPortW + portLabelW + Theme::Spacing::M * 2));
    formChanged |= ImGui::InputTextWithHint("##ssh_host", "SSH server hostname", sshHostBuf_,
                                            sizeof(sshHostBuf_));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kPortW);
    formChanged |= ImGui::InputText("##ssh_port", sshPortBuf_, sizeof(sshPortBuf_),
                                    ImGuiInputTextFlags_CharsDecimal);

    fieldLabel("SSH User");
    ImGui::SetNextItemWidth(-FLT_MIN);
    formChanged |= ImGui::InputTextWithHint("##ssh_user", "SSH username", sshUsernameBuf_,
                                            sizeof(sshUsernameBuf_));

    fieldLabel("SSH Auth");
    formChanged |= ImGui::RadioButton("Password##ssh_auth", &sshAuthIdx_, 0);
    ImGui::SameLine();
    formChanged |= ImGui::RadioButton("Private Key##ssh_auth", &sshAuthIdx_, 1);

    if (sshAuthIdx_ == 0) {
        fieldLabel("SSH Pass");
        ImGui::SetNextItemWidth(-FLT_MIN);
        formChanged |=
            ImGui::InputTextWithHint("##ssh_password", "SSH password", sshPasswordBuf_,
                                     sizeof(sshPasswordBuf_), ImGuiInputTextFlags_Password);
    } else {
        fieldLabel("Key File");
        float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(-(browseW + Theme::Spacing::M));
        formChanged |= ImGui::InputTextWithHint("##ssh_key", "~/.ssh/id_rsa", sshKeyPathBuf_,
                                                sizeof(sshKeyPathBuf_));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##sshkey")) {
            std::string path = FileDialog::openFile();
            if (!path.empty()) {
                copyToBuf(sshKeyPathBuf_, sizeof(sshKeyPathBuf_), path);
                formChanged = true;
            }
        }

        fieldLabel("Passphrase");
        ImGui::SetNextItemWidth(-FLT_MIN);
        formChanged |= ImGui::InputTextWithHint("##ssh_passphrase", "Key passphrase (optional)",
                                                sshPasswordBuf_, sizeof(sshPasswordBuf_),
                                                ImGuiInputTextFlags_Password);
    }
}

void ConnectionDialog::renderStatusRow() {
    const auto& colors = Application::getInstance().getCurrentColors();

    if (isBusy()) {
        UIUtils::Spinner("##conn_spinner", 7.0f, 3, ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SameLine();
    }

    if (!statusText_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, statusIsError_ ? colors.red : colors.subtext0);
        ImGui::TextWrapped("%s", statusText_.c_str());
        ImGui::PopStyleColor();
    } else if (!isBusy()) {
        // keep the row height stable
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }
}

bool ConnectionDialog::renderButtons() {
    const bool editing = (editingConnectionId_ != -1);

    float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(rightEdge - kButtonW * 2 - Theme::Spacing::M);

    if (ImGui::Button("Cancel##conn", ImVec2(kButtonW, 0))) {
        finishClose(true);
        ImGui::CloseCurrentPopup();
        return false;
    }

    ImGui::SameLine();

    bool clicked = false;
    ImGui::BeginDisabled(isBusy());
    const auto& colors = Application::getInstance().getCurrentColors();
    const ImVec4 green = colors.green;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(green.x, green.y, green.z, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, green);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(green.x * 0.85f, green.y * 0.85f, green.z * 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.base);
    if (ImGui::Button(editing ? "Update##conn" : "Connect##conn", ImVec2(kButtonW, 0)))
        clicked = true;
    ImGui::PopStyleColor(4);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        clicked = true;
    ImGui::EndDisabled();

    return clicked && !isBusy();
}
