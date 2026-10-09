#pragma once

#include <optional>

// `dearsql --tui` / `--mcp` / `--help`; nullopt when argv is for the GUI
std::optional<int> runCli(int argc, char** argv);
