#pragma once

#include "cli/connections.hpp"
#include <chrono>
#include <dearsql/completion.hpp>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>

// `dearsql --lsp`: sql completion and hover for editors over the language
// server protocol. transport-free so tests can drive it: messages go in
// through handle(), replies and notifications come out through `send`.
class LspServer {
public:
    using Send = std::function<void(const nlohmann::json&)>;

    // defaultConnection: a connection name from the command line, or ""
    LspServer(CliConnections& connections, std::string defaultConnection, Send send);

    // one json-rpc message in; false once `exit` arrived
    bool handle(const nlohmann::json& message);
    // process exit code after `exit`: 0 when `shutdown` came first
    int exitCode() const {
        return shutdown_ ? 0 : 1;
    }

private:
    struct Target {
        std::string connection; // spec: saved name, url or file
        std::string database;
    };
    struct Catalog {
        std::chrono::steady_clock::time_point loaded;
        dearsql::CompletionCatalog catalog;
        dearsql::DatabaseType type = dearsql::DatabaseType::POSTGRESQL;
        std::string error; // set when the connection or catalog failed
    };

    nlohmann::json initialize(const nlohmann::json& params);
    nlohmann::json completion(const nlohmann::json& params);
    nlohmann::json hover(const nlohmann::json& params);

    nlohmann::json executeCommand(const nlohmann::json& params);

    std::optional<Target> targetFor(const std::string& uri, const std::string& text) const;
    std::optional<Target> serverTarget() const;
    // cached for two minutes; null when no connection is configured
    const Catalog* catalogFor(const std::string& uri, const std::string& text);
    const Catalog* catalogFor(const Target& target);
    void warnOnce(const std::string& message);

    CliConnections& connections_;
    std::string defaultConnection_;
    Target initOptions_;
    std::optional<Target> switched_; // set by switchConnections / switchDatabase
    Send send_;
    bool shutdown_ = false;
    std::map<std::string, std::string> documents_; // uri -> text
    std::map<std::string, Catalog> catalogs_;      // "connection\ndatabase" -> catalog
    std::set<std::string> warned_;
};

// serve on stdin/stdout until `exit` or eof
int runLsp(CliConnections& connections, const std::string& defaultConnection);

// lsp positions count utf-16 code units; byte offsets into utf-8 text (exposed for tests)
size_t lspOffsetAt(const std::string& text, int line, int character);
std::pair<int, int> lspPositionAt(const std::string& text, size_t offset);
