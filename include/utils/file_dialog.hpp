#pragma once

#include "database/db_interface.hpp"
#include <functional>
#include <string>

// the app's dialogs over mydear/file_dialog.hpp. done gets the path, never when cancelled:
// at once on the desktop, later for an open on mobile, where the picker is not modal
class FileDialog {
public:
    using Done = std::function<void(const std::string&)>;
    static void openDatabaseFile(DatabaseType type, Done done);
    static void openFile(Done done);
    static void openCSVFile(Done done);
    static void openPostgresBackupFile(Done done);

    static void pickFolder(Done done);
    static void savePostgresBackupFile(const std::string& defaultName, Done done);
    // one filter, e.g. ("CSV Files", "csv"), or none (nullptr)
    static void saveFile(const std::string& defaultName, const char* filterName,
                         const char* filterSpec, Done done);
};
