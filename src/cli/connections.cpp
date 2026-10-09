#include "cli/connections.hpp"

#include "app_state.hpp"
#include "database/connection_url.hpp"
#include <dearsql/factory.hpp>
#include <filesystem>

CliConnections::~CliConnections() {
    for (auto& e : entries_) {
        if (e->conn)
            e->conn->close();
    }
}

bool CliConnections::loadSaved(std::string& error) {
    AppState state;
    if (!state.initialize()) {
        error = "could not read ~/.dearsql/connections.db";
        return false;
    }
    std::lock_guard lock(mutex_);
    for (auto& saved : state.getSavedConnections()) {
        if (find(saved.connectionInfo.name))
            continue;
        auto e = std::make_unique<Entry>();
        e->info = std::move(saved.connectionInfo);
        entries_.push_back(std::move(e));
    }
    return true;
}

std::string CliConnections::add(const std::string& spec, std::string& error) {
    std::lock_guard lock(mutex_);
    if (auto* e = find(spec))
        return e->info.name;

    DatabaseConnectionInfo info;
    if (looksLikeConnectionUrl(spec)) {
        auto parsed = parseConnectionUrl(spec);
        if (!parsed.ok) {
            error = parsed.error;
            return "";
        }
        info = std::move(parsed.info);
        if (info.name.empty())
            info.name = info.host.empty()
                            ? spec
                            : info.host + (info.database.empty() ? "" : "/" + info.database);
    } else if (std::filesystem::exists(spec)) {
        const auto ext = std::filesystem::path(spec).extension().string();
        info.type = (ext == ".duckdb" || ext == ".ddb" || dearsql::isCsvPath(spec))
                        ? DatabaseType::DUCKDB
                        : DatabaseType::SQLITE;
        info.path = std::filesystem::absolute(spec).string();
        info.name = std::filesystem::path(spec).filename().string();
    } else {
        error = "'" + spec + "' is not a saved connection, a connection URL, or a file";
        return "";
    }
    auto e = std::make_unique<Entry>();
    e->info = std::move(info);
    e->spec = spec;
    entries_.push_back(std::move(e));
    return entries_.back()->info.name;
}

std::vector<CliConnections::Entry*> CliConnections::entries() {
    std::lock_guard lock(mutex_);
    std::vector<Entry*> out;
    for (auto& e : entries_)
        out.push_back(e.get());
    return out;
}

CliConnections::Entry* CliConnections::find(const std::string& name) {
    std::lock_guard lock(mutex_);
    for (auto& e : entries_) {
        if (e->info.name == name || (!e->spec.empty() && e->spec == name))
            return e.get();
    }
    return nullptr;
}

std::string CliConnections::open(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto* e = find(name);
    if (!e)
        return "no connection named '" + name + "'";
    if (e->conn)
        return "";

    dearsql::ConnectionInfo info = e->info;
    if (e->info.ssh.enabled) {
        e->tunnel = std::make_unique<SSHTunnel>();
        auto [ok, err] = e->tunnel->start(e->info.ssh, e->info.host, e->info.port);
        if (!ok)
            return "ssh tunnel: " + err;
        info.host = "127.0.0.1";
        info.port = e->tunnel->localPort();
    }
    auto conn = dearsql::makeConnection(info);
    if (!conn)
        return "unsupported database type";
    if (auto [ok, err] = conn->open(); !ok) {
        e->tunnel.reset();
        return err;
    }
    e->conn = std::move(conn);
    return "";
}

std::shared_ptr<dearsql::IConnection> CliConnections::connection(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto* e = find(name);
    return e ? e->conn : nullptr;
}
