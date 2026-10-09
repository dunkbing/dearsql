#pragma once

#include <string>

// the `dearsql` shell command: a symlink on PATH to this executable, so
// `dearsql`, `dearsql --tui` and `dearsql --mcp` work from a terminal
namespace CliCommand {

    // the app can install it itself (macOS, Linux); Windows' installer owns PATH
    bool supported();
    // /usr/local/bin/dearsql on macOS, ~/.local/bin/dearsql on Linux, or $DEARSQL_CLI_LINK
    std::string linkPath();
    // the link exists and points at this executable
    bool installed();
    // creates (or repoints) the link, asking for an admin password on macOS when
    // /usr/local/bin is not writable. "" on success, else the error
    std::string install();

} // namespace CliCommand
