#pragma once

#include "database/db_interface.hpp"
#include <memory>

class DatabaseInterface;

class FileDialog {
public:
    FileDialog() = default;
    ~FileDialog() = default;

    // Initialize/cleanup NFD
    static bool initialize();
    static void cleanup();

    // File operations only
    static std::shared_ptr<DatabaseInterface>
    openDatabaseFile(DatabaseType type = DatabaseType::SQLITE);

    // returns the selected file path, or empty string if cancelled
    static std::string openFile();
    static std::string pickFolder();
    static std::string openCSVFile();
    static std::string openPostgresBackupFile();
    static std::string savePostgresBackupFile(const std::string& defaultName);

private:
    static bool isInitialized;
};
