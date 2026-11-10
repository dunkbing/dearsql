// android file dialogs: the system document picker copies the file into the app's documents
// folder (another app's files are not reachable by path); saves land there too
#include "mydear/android.hpp"
#include "utils/cli_command.hpp"
#include "utils/file_dialog.hpp"

#include <cstdlib>
#include <filesystem>

namespace {
    std::string documents() {
        const char* home = std::getenv("HOME");
        std::filesystem::path dir = std::filesystem::path(home ? home : ".") / "Documents";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }

    // shortcut: the copy is what the app opens; edits do not reach the picked original
    void pick(const char* mimeTypes, FileDialog::Done done) {
        mydear::PickFile(mimeTypes, documents(), std::move(done));
    }

    // a free name in documents: name, then "name 2.ext", ...
    std::string freePath(const std::string& name) {
        const std::filesystem::path base = std::filesystem::path(documents()) / name;
        std::filesystem::path p = base;
        for (int i = 2; std::filesystem::exists(p); ++i)
            p = base.parent_path() /
                (base.stem().string() + " " + std::to_string(i) + base.extension().string());
        return p.string();
    }
} // namespace

bool FileDialog::isInitialized = true;

bool FileDialog::initialize() {
    return true;
}

void FileDialog::cleanup() {}

void FileDialog::openDatabaseFile(DatabaseType, Done done) {
    pick("", std::move(done)); // sqlite files have no reliable mime type
}

void FileDialog::openFile(Done done) {
    pick("", std::move(done));
}

void FileDialog::openCSVFile(Done done) {
    pick("text/csv,text/comma-separated-values,text/plain", std::move(done));
}

void FileDialog::openPostgresBackupFile(Done done) {
    pick("", std::move(done));
}

std::string FileDialog::pickFolder() {
    return documents();
}

std::string FileDialog::savePostgresBackupFile(const std::string& defaultName) {
    return freePath(defaultName);
}

std::string FileDialog::saveFile(const std::string& defaultName, const char*, const char*) {
    return freePath(defaultName);
}

// no terminal on android
namespace CliCommand {
    bool supported() {
        return false;
    }
    std::string executable() {
        return {};
    }
    std::string linkPath() {
        return {};
    }
    bool installed() {
        return false;
    }
    std::string install() {
        return "Not available on Android.";
    }
} // namespace CliCommand
