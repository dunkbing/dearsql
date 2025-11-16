#include "utils/file_dialog.hpp"
#include "database/sqlite.hpp"
#include <iostream>
#include <nfd.h>

bool FileDialog::isInitialized = false;

bool FileDialog::initialize() {
    if (!isInitialized) {
        NFD_Init();
        isInitialized = true;
    }
    return true;
}

void FileDialog::cleanup() {
    if (isInitialized) {
        NFD_Quit();
        isInitialized = false;
    }
}

std::shared_ptr<DatabaseInterface> FileDialog::openSQLiteFile() {
    nfdchar_t* outPath;
    constexpr nfdfilteritem_t filterItem[2] = {{"SQLite Database", "db,sqlite,sqlite3"},
                                               {"All Files", "*"}};

    const nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 2, nullptr);
    if (result == NFD_OKAY) {
        std::string path(outPath);
        const size_t lastSlash = path.find_last_of("/\\");
        std::string name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

        DatabaseConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.name = name;
        info.path = path;

        auto db = std::make_shared<SQLiteDatabase>(info);
        NFD_FreePath(outPath);
        return db;
    }
    if (result == NFD_CANCEL) {
        return nullptr;
    }
    std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    return nullptr;
}
