#pragma once

#include "cli/connections.hpp"

// full-screen terminal client; initial = a connection name to expand on start
int runTui(CliConnections& connections, const std::string& initial);
