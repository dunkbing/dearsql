#include "utils/file_dialog.hpp"
#include "database/db_interface.hpp"
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

std::shared_ptr<DatabaseInterface> FileDialog::openDatabaseFile(DatabaseType type) {
    nfdchar_t* outPath;
    const nfdfilteritem_t filterItem[2] = {
        type == DatabaseType::DUCKDB ? nfdfilteritem_t{"DuckDB Database", "duckdb,ddb,db"}
                                     : nfdfilteritem_t{"SQLite Database", "db,sqlite,sqlite3"},
        {"All Files", "*"}};

    const nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 2, nullptr);
    if (result == NFD_OKAY) {
        std::string path(outPath);
        const size_t lastSlash = path.find_last_of("/\\");
        std::string name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

        DatabaseConnectionInfo info;
        info.type = type;
        info.name = name;
        info.path = path;

        auto db = DatabaseFactory::createDatabase(info);
        NFD_FreePath(outPath);
        return db;
    }
    if (result == NFD_CANCEL) {
        return nullptr;
    }
    std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    return nullptr;
}

std::string FileDialog::openFile() {
    nfdchar_t* outPath;
    const nfdresult_t result = NFD_OpenDialog(&outPath, nullptr, 0, nullptr);
    if (result == NFD_OKAY) {
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }
    if (result != NFD_CANCEL) {
        std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    }
    return {};
}

std::string FileDialog::pickFolder() {
    nfdchar_t* outPath;
    const nfdresult_t result = NFD_PickFolder(&outPath, nullptr);
    if (result == NFD_OKAY) {
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }
    if (result != NFD_CANCEL) {
        std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    }
    return {};
}

std::string FileDialog::openCSVFile() {
    nfdchar_t* outPath;
    constexpr nfdfilteritem_t filterItem[2] = {{"CSV Files", "csv"}, {"All Files", "*"}};

    const nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 2, nullptr);
    if (result == NFD_OKAY) {
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }
    if (result != NFD_CANCEL) {
        std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    }
    return {};
}

std::string FileDialog::openPostgresBackupFile() {
    nfdchar_t* outPath;
    constexpr nfdfilteritem_t filterItem[3] = {
        {"PostgreSQL Backups", "dump,backup,sql,tar"}, {"SQL Files", "sql"}, {"All Files", "*"}};

    const nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 3, nullptr);
    if (result == NFD_OKAY) {
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }
    if (result != NFD_CANCEL) {
        std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    }
    return {};
}

std::string FileDialog::savePostgresBackupFile(const std::string& defaultName) {
    nfdchar_t* outPath;
    constexpr nfdfilteritem_t filterItem[3] = {
        {"PostgreSQL Custom Backup", "dump,backup"}, {"SQL Files", "sql"}, {"All Files", "*"}};

    const nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, 3, nullptr,
                                              defaultName.empty() ? nullptr : defaultName.c_str());
    if (result == NFD_OKAY) {
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }
    if (result != NFD_CANCEL) {
        std::cerr << "File dialog error: " << NFD_GetError() << std::endl;
    }
    return {};
}
