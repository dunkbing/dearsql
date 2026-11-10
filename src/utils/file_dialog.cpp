#include "utils/file_dialog.hpp"
#include "mydear/app.hpp"
#include "mydear/file_dialog.hpp"

namespace {
    // keys, certificates, uploads: copied out of sight on mobile (the files app shows the
    // documents folder, where databases and csv files go)
    std::string imported() {
        return mydear::DocumentsDir().empty() ? std::string() : mydear::DataDir("Imported");
    }
} // namespace

void FileDialog::openDatabaseFile(const DatabaseType type, Done done) {
    mydear::OpenFileDialog({type == DatabaseType::DUCKDB
                                ? mydear::FileFilter{"DuckDB Database", "duckdb,ddb,db"}
                                : mydear::FileFilter{"SQLite Database", "db,sqlite,sqlite3"},
                            {"All Files", "*"}},
                           std::move(done));
}

void FileDialog::openFile(Done done) {
    mydear::OpenFileDialog({}, std::move(done), imported());
}

void FileDialog::openCSVFile(Done done) {
    mydear::OpenFileDialog({{"CSV Files", "csv"}, {"All Files", "*"}}, std::move(done));
}

void FileDialog::openPostgresBackupFile(Done done) {
    mydear::OpenFileDialog(
        {{"PostgreSQL Backups", "dump,backup,sql,tar"}, {"SQL Files", "sql"}, {"All Files", "*"}},
        std::move(done), imported());
}

void FileDialog::pickFolder(Done done) {
    mydear::PickFolderDialog(std::move(done));
}

void FileDialog::savePostgresBackupFile(const std::string& defaultName, Done done) {
    mydear::SaveFileDialog(
        defaultName,
        {{"PostgreSQL Custom Backup", "dump,backup"}, {"SQL Files", "sql"}, {"All Files", "*"}},
        std::move(done));
}

void FileDialog::saveFile(const std::string& defaultName, const char* filterName,
                          const char* filterSpec, Done done) {
    std::vector<mydear::FileFilter> filters;
    if (filterName)
        filters.push_back({filterName, filterSpec});
    mydear::SaveFileDialog(defaultName, filters, std::move(done));
}
