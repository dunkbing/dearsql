#pragma once

#include "database/db_interface.hpp"
#include <memory>
#include <string>

class DatabaseInterface;

class DatabaseConnectionDialog {
public:
    DatabaseConnectionDialog() = default;
    ~DatabaseConnectionDialog() = default;

    // Main dialog function - call this from UI
    void showDialog();

    // Check if dialog is currently open
    bool isDialogOpen() const { return isOpen; }

    // Get the result (will be nullptr if dialog cancelled or not completed)
    std::shared_ptr<DatabaseInterface> getResult();

    // Reset dialog state
    void reset();

private:
    // Dialog state
    bool isOpen = false;
    bool showingTypeSelection = false;
    bool showingPostgreSQLConnection = false;
    bool isConnecting = false;
    std::string errorMessage;

    // Selected database type
    int selectedDatabaseType = 0; // 0 = SQLite, 1 = PostgreSQL

    // PostgreSQL connection fields
    char connectionName[256] = "";
    char host[256] = "localhost";
    int port = 5432;
    char database[256] = "";
    char username[256] = "";
    char password[256] = "";

    // Result
    std::shared_ptr<DatabaseInterface> result = nullptr;

    // Dialog rendering functions
    void renderTypeSelection();
    void renderPostgreSQLConnection();

    // Helper functions
    static std::shared_ptr<DatabaseInterface> createSQLiteDatabase();
    std::shared_ptr<DatabaseInterface> createPostgreSQLDatabase();
};
