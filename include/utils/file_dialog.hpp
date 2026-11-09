#pragma once

#include "database/db_interface.hpp"
#include <functional>
#include <string>

class FileDialog {
public:
    // Initialize/cleanup NFD
    static bool initialize();
    static void cleanup();

    // open dialogs call done with the picked path, never when cancelled: at once on the
    // desktop, later on ios, where the picker is not modal
    using Done = std::function<void(const std::string&)>;
    static void openDatabaseFile(DatabaseType type, Done done);
    static void openFile(Done done);
    static void openCSVFile(Done done);
    static void openPostgresBackupFile(Done done);

    // returns the selected path, or empty string if cancelled
    static std::string pickFolder();
    static std::string savePostgresBackupFile(const std::string& defaultName);
    // save dialog with one filter, e.g. ("CSV Files", "csv"), or none (nullptr); empty when
    // cancelled
    static std::string saveFile(const std::string& defaultName, const char* filterName,
                                const char* filterSpec);

private:
    static bool isInitialized;
};
