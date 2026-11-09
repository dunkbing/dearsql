#include "utils/file_dialog.hpp"

// ipad: no native dialogs yet. ponytail: every picker returns nothing until the
// document picker (UIDocumentPickerViewController) is wired up
bool FileDialog::isInitialized = true;

bool FileDialog::initialize() {
    return true;
}
void FileDialog::cleanup() {}
std::shared_ptr<DatabaseInterface> FileDialog::openDatabaseFile(DatabaseType) {
    return nullptr;
}
std::string FileDialog::openFile() {
    return {};
}
std::string FileDialog::pickFolder() {
    return {};
}
std::string FileDialog::openCSVFile() {
    return {};
}
std::string FileDialog::openPostgresBackupFile() {
    return {};
}
std::string FileDialog::savePostgresBackupFile(const std::string&) {
    return {};
}
std::string FileDialog::saveFile(const std::string&, const char*, const char*) {
    return {};
}
