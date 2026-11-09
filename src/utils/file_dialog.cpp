#include "utils/file_dialog.hpp"
#include "database/db_interface.hpp"
#include <iostream>
#include <nfd.h>
#include <spdlog/spdlog.h>

bool FileDialog::isInitialized = false;

bool FileDialog::initialize() {
    // fails without a dbus session bus on linux; NFD_Quit then aborts in dbus
    if (!isInitialized) {
        isInitialized = NFD_Init() == NFD_OKAY;
        if (!isInitialized)
            spdlog::warn("file dialogs unavailable: {}", NFD_GetError());
    }
    return true;
}

void FileDialog::cleanup() {
    if (isInitialized) {
        NFD_Quit();
        isInitialized = false;
    }
}

namespace {
    void open(const nfdfilteritem_t* filters, const nfdfiltersize_t count,
              const FileDialog::Done& done) {
        nfdchar_t* outPath;
        const nfdresult_t result = NFD_OpenDialog(&outPath, filters, count, nullptr);
        if (result == NFD_OKAY) {
            const std::string path(outPath);
            NFD_FreePath(outPath);
            done(path);
        } else if (result != NFD_CANCEL) {
            spdlog::error("file dialog: {}", NFD_GetError());
        }
    }
} // namespace

void FileDialog::openDatabaseFile(const DatabaseType type, Done done) {
    const nfdfilteritem_t filterItem[2] = {
        type == DatabaseType::DUCKDB ? nfdfilteritem_t{"DuckDB Database", "duckdb,ddb,db"}
                                     : nfdfilteritem_t{"SQLite Database", "db,sqlite,sqlite3"},
        {"All Files", "*"}};
    open(filterItem, 2, done);
}

void FileDialog::openFile(Done done) {
    open(nullptr, 0, done);
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

void FileDialog::openCSVFile(Done done) {
    constexpr nfdfilteritem_t filterItem[2] = {{"CSV Files", "csv"}, {"All Files", "*"}};
    open(filterItem, 2, done);
}

void FileDialog::openPostgresBackupFile(Done done) {
    constexpr nfdfilteritem_t filterItem[3] = {
        {"PostgreSQL Backups", "dump,backup,sql,tar"}, {"SQL Files", "sql"}, {"All Files", "*"}};
    open(filterItem, 3, done);
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

std::string FileDialog::saveFile(const std::string& defaultName, const char* filterName,
                                 const char* filterSpec) {
    nfdchar_t* outPath;
    const nfdfilteritem_t filterItem[1] = {{filterName, filterSpec}};
    const nfdresult_t result = NFD_SaveDialog(&outPath, filterItem, filterName ? 1 : 0, nullptr,
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
