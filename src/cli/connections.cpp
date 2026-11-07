#include "cli/connections.hpp"

#include "app_state.hpp"
#include "database/connection_url.hpp"
#include "license/license_manager.hpp"
#include <dearsql/factory.hpp>
#include <filesystem>
#include <format>

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
        // object storage is gui-only: the terminal modes are database tools
        if (saved.connectionInfo.type == DatabaseType::S3 || find(saved.connectionInfo.name))
            continue;
        auto e = std::make_unique<Entry>();
        e->info = std::move(saved.connectionInfo);
        e->saved = true;
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
    // a url or file already saved: use the saved one (its name, its ssh settings)
    for (auto& s : entries_) {
        const auto& o = s->info;
        if (s->saved && o.type == info.type &&
            (info.path.empty() ? o.host == info.host && o.port == info.port &&
                                     o.database == info.database && o.username == info.username
                               : o.path == info.path))
            return o.name;
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

std::string CliConnections::save(const std::string& name, std::string& error) {
    std::lock_guard lock(mutex_);
    auto* e = find(name);
    if (!e || e->saved)
        return "";
    AppState state;
    if (!state.initialize()) {
        error = "could not open ~/.dearsql/connections.db";
        return "";
    }
    int workspace = 1;
    try {
        workspace = std::stoi(state.getSetting("current_workspace", "1"));
    } catch (const std::exception&) {
    }
    // the stored license, as LicenseManager::loadStoredLicense reads it (that needs the gui)
    const bool pro = !state.getSetting("license_key", "").empty() &&
                     state.getSetting("license_status", "") == "active";
    if (!pro &&
        state.getConnectionsForWorkspace(workspace).size() >= kFreeConnectionLimit) {
        error = std::format("not saved: the free tier keeps {} connections", kFreeConnectionLimit);
        return "";
    }
    SavedConnection conn;
    conn.connectionInfo = e->info;
    conn.workspaceId = workspace;
    if (state.saveConnection(conn) <= 0) {
        error = "could not save the connection";
        return "";
    }
    e->saved = true;
    return "Saved as '" + e->info.name + "', so it shows in DearSQL too.";
}
