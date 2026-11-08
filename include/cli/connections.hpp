#pragma once

#include "database/db_interface.hpp"
#include "database/s3_client.hpp"
#include "database/ssh_tunnel.hpp"
#include <map>
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
        std::string spec; // the URL or path it was added from, so a repeat finds it
        std::shared_ptr<dearsql::IConnection> conn; // null until opened
        std::unique_ptr<SSHTunnel> tunnel;
        bool saved = false; // one of the GUI's saved connections
    };

    ~CliConnections();

    // the GUI's saved connections (~/.dearsql/connections.db); false + error if unreadable
    bool loadSaved(std::string& error);
    // a connection URL, a .sqlite/.duckdb/.csv path, or a saved connection name.
    // returns the entry's name, or "" with error set
    std::string add(const std::string& spec, std::string& error);
    // adds name to the GUI's saved connections (current workspace) unless it is one:
    // a note for the user, "" if it was saved already, or "" with error set
    std::string save(const std::string& name, std::string& error);

    std::vector<Entry*> entries();
    // by name, or by the URL/path it was added from
    Entry* find(const std::string& name);
    // "" on success, else the error
    std::string open(const std::string& name);
    std::shared_ptr<dearsql::IConnection> connection(const std::string& name);
    // saved S3 connections: not database connections, so kept apart from entries()
    [[nodiscard]] const std::vector<DatabaseConnectionInfo>& s3Connections() const {
        return s3_;
    }
    std::shared_ptr<const S3Client> s3(const std::string& name);

private:
    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<DatabaseConnectionInfo> s3_;
    std::map<std::string, std::shared_ptr<const S3Client>> s3Clients_;
    std::recursive_mutex mutex_;
};
