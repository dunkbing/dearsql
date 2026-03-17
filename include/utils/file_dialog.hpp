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
    static std::shared_ptr<DatabaseInterface> openSQLiteFile();

    // returns the selected file path, or empty string if cancelled
    static std::string openCSVFile();

private:
    static bool isInitialized;
};
