// no terminal on android (file dialogs: imshell/file_dialog.hpp)
#include "utils/cli_command.hpp"

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
