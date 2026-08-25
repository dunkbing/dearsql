#pragma once

#include "database/async_helper.hpp"
#include "database/db_interface.hpp"
#include "database/oracle/oracle_client_installer.hpp"
#include <memory>
#include <string>

class Application;

// imgui-based connection dialog. cross-platform replacement for the
// platform-specific dialogs (src/platform/*_connection_dialog.*); render()
// must be called once per frame from the main render loop.
class ConnectionDialog {
public:
    static ConnectionDialog& instance();

    ConnectionDialog(const ConnectionDialog&) = delete;
    ConnectionDialog& operator=(const ConnectionDialog&) = delete;

    void show(Application* app);
    void showEdit(Application* app, std::shared_ptr<DatabaseInterface> db);
    void render();

    [[nodiscard]] bool isOpen() const {
        return open_;
    }

private:
    ConnectionDialog() = default;
    ~ConnectionDialog() = default;

    struct ConnectResult {
        bool success = false;
        std::string error;
        std::shared_ptr<DatabaseInterface> db;
        DatabaseConnectionInfo info;
    };

    [[nodiscard]] DatabaseType selectedType() const {
        return static_cast<DatabaseType>(typeIdx_);
    }
    [[nodiscard]] bool isBusy() const {
        return connectOp_.isRunning() || installingOracle_;
    }

    void resetForm();
    void applyTypeDefaults(DatabaseType type);
    void populateForm(const DatabaseConnectionInfo& info);
    [[nodiscard]] DatabaseConnectionInfo snapshotForm() const;
    void rebuildUrlFromForm();
    void applyUrlToForm();
    void setStatus(const std::string& text, bool isError);

    void startConnect();
    void connectFileDatabase();
    void startServerConnect();
    void pollConnect();
    void pollOracleInstall();
    void handleSuccess(const std::shared_ptr<DatabaseInterface>& db,
                       const DatabaseConnectionInfo& info);
    void finishClose(bool cancelled);

    void renderTypeRow();
    void renderUrlRow();
    void renderSqliteFields(bool& formChanged);
    void renderServerFields(bool& formChanged);
    void renderSshFields(bool& formChanged);
    void renderStatusRow();
    bool renderButtons();

    Application* app_ = nullptr;
    bool open_ = false;
    bool pendingOpen_ = false;
    bool closeRequested_ = false;
    // set when the dialog is dismissed mid-connect so the late result is discarded
    bool cancelled_ = false;

    std::shared_ptr<DatabaseInterface> editingDb_;
    int editingConnectionId_ = -1;

    // form state
    int typeIdx_ = 0;
    int connectByIdx_ = 0; // 0 = host, 1 = url
    char nameBuf_[256] = "Untitled connection";
    char urlBuf_[1024] = {};
    std::string urlError_;
    char sqlitePathBuf_[1024] = {};
    char hostBuf_[256] = "localhost";
    char portBuf_[16] = "5432";
    char databaseBuf_[256] = {};
    int sslModeIdx_ = 0;
    char sslCACertPathBuf_[1024] = {};
    int authIdx_ = 0; // 0 = username & password, 1 = none
    char usernameBuf_[256] = {};
    char passwordBuf_[256] = {};
    bool showAllDbs_ = false;

    // ssh tunnel
    bool sshEnabled_ = false;
    char sshHostBuf_[256] = {};
    char sshPortBuf_[16] = "22";
    char sshUsernameBuf_[256] = {};
    int sshAuthIdx_ = 0; // 0 = password, 1 = private key
    char sshPasswordBuf_[256] = {};
    char sshKeyPathBuf_[1024] = {};

    std::string statusText_;
    bool statusIsError_ = false;

    AsyncOperation<ConnectResult> connectOp_;
    OracleClientInstaller oracleInstaller_;
    bool installingOracle_ = false;
};
