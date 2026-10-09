#pragma once

#include "database/db_interface.hpp"
#include "database/ssh_tunnel.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// named connections for the cli: the GUI's saved connections plus any URL or
// file given on the command line. opened on demand, synchronously, through
// libdearsql; ssh tunnels via the same SSHTunnel the GUI uses.
class CliConnections {
public:
    struct Entry {
        DatabaseConnectionInfo info;
        std::shared_ptr<dearsql::IConnection> conn; // null until opened
        std::unique_ptr<SSHTunnel> tunnel;
    };

    ~CliConnections();

    // the GUI's saved connections (~/.dearsql/connections.db); false + error if unreadable
    bool loadSaved(std::string& error);
    // a connection URL, a .sqlite/.duckdb/.csv path, or a saved connection name.
    // returns the entry's name, or "" with error set
    std::string add(const std::string& spec, std::string& error);

    std::vector<Entry*> entries();
    Entry* find(const std::string& name);
    // "" on success, else the error
    std::string open(const std::string& name);
    std::shared_ptr<dearsql::IConnection> connection(const std::string& name);

private:
    std::vector<std::unique_ptr<Entry>> entries_;
    std::recursive_mutex mutex_;
};
