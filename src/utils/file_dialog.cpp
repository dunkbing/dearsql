#include "utils/file_dialog.hpp"
#include "imshell/app.hpp"
#include "imshell/file_dialog.hpp"

namespace {
    // keys, certificates, uploads: copied out of sight on mobile (the files app shows the
    // documents folder, where databases and csv files go)
    std::string imported() {
        return imshell::DocumentsDir().empty() ? std::string() : imshell::DataDir("Imported");
    }
} // namespace

void FileDialog::openDatabaseFile(const DatabaseType type, Done done) {
    imshell::OpenFileDialog({type == DatabaseType::DUCKDB
                                ? imshell::FileFilter{"DuckDB Database", "duckdb,ddb,db"}
                                : imshell::FileFilter{"SQLite Database", "db,sqlite,sqlite3"},
                            {"All Files", "*"}},
                           std::move(done));
}

void FileDialog::openFile(Done done) {
    imshell::OpenFileDialog({}, std::move(done), imported());
}

void FileDialog::openCSVFile(Done done) {
    imshell::OpenFileDialog({{"CSV Files", "csv"}, {"All Files", "*"}}, std::move(done));
}

void FileDialog::openPostgresBackupFile(Done done) {
    imshell::OpenFileDialog(
        {{"PostgreSQL Backups", "dump,backup,sql,tar"}, {"SQL Files", "sql"}, {"All Files", "*"}},
        std::move(done), imported());
}

void FileDialog::pickFolder(Done done) {
    imshell::PickFolderDialog(std::move(done));
}

void FileDialog::savePostgresBackupFile(const std::string& defaultName, Done done) {
    imshell::SaveFileDialog(
        defaultName,
        {{"PostgreSQL Custom Backup", "dump,backup"}, {"SQL Files", "sql"}, {"All Files", "*"}},
        std::move(done));
}

void FileDialog::saveFile(const std::string& defaultName, const char* filterName,
                          const char* filterSpec, Done done) {
    std::vector<imshell::FileFilter> filters;
    if (filterName)
        filters.push_back({filterName, filterSpec});
    imshell::SaveFileDialog(defaultName, filters, std::move(done));
}
