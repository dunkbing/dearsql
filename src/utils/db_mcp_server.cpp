#include "utils/db_mcp_server.hpp"

#include "database/database_node.hpp"
#include "utils/sql_guard.hpp"
#include <chrono>
#include <format>
#include <random>
#include <spdlog/spdlog.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

using json = nlohmann::json;

namespace {
    std::string makeToken() {
        static constexpr char HEX[] = "0123456789abcdef";
        std::random_device rd;
        std::uniform_int_distribution<int> dist(0, 15);
        std::string token;
        token.reserve(32);
        for (int i = 0; i < 32; ++i) {
            token += HEX[dist(rd)];
        }
        return token;
    }

    constexpr int MAX_ROWS = 200;
    constexpr size_t MAX_TEXT = 60 * 1024;

    json textResult(const std::string& text, bool isError = false) {
        return {{"content", json::array({{{"type", "text"}, {"text", text}}})},
                {"isError", isError}};
    }

    std::string formatQueryResult(const QueryResult& result) {
        std::string out;
        for (const auto& stmt : result.statements) {
            if (!stmt.success) {
                out += "Error: " + stmt.errorMessage + "\n";
                continue;
            }
            if (!stmt.columnNames.empty()) {
                for (size_t i = 0; i < stmt.columnNames.size(); ++i) {
                    if (i > 0) {
                        out += "\t";
                    }
                    out += stmt.columnNames[i];
                }
                out += "\n";
                int rows = 0;
                for (const auto& row : stmt.tableData) {
                    if (++rows > MAX_ROWS) {
                        out += std::format("... ({} more rows truncated)\n",
                                           stmt.tableData.size() - MAX_ROWS);
                        break;
                    }
                    for (size_t i = 0; i < row.size(); ++i) {
                        if (i > 0) {
                            out += "\t";
                        }
                        out += row[i];
                    }
                    out += "\n";
                    if (out.size() > MAX_TEXT) {
                        out += "... (output truncated)\n";
                        return out;
                    }
                }
                out += std::format("({} rows)\n", stmt.tableData.size());
            } else if (!stmt.message.empty()) {
                out += stmt.message + "\n";
            }
        }
        if (out.empty()) {
            out = "(no results)";
        }
        return out;
    }
} // namespace

DbMcpServer::DbMcpServer() = default;

DbMcpServer::~DbMcpServer() {
    stop();
}

bool DbMcpServer::start() {
    if (server_) {
        return true;
    }
    server_ = std::make_unique<httplib::Server>();
    token_ = makeToken();
    stopping_ = false;

    server_->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        // set before the listener thread starts, so reading it here needs no lock
        if (token_.empty() || req.get_header_value("Authorization") != "Bearer " + token_) {
            res.status = 401;
            return;
        }
        json rpc;
        try {
            rpc = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            return;
        }
        if (!rpc.contains("id")) {
            res.status = 202; // notification
            return;
        }
        res.set_content(handleRpc(rpc).dump(), "application/json");
    });

    port_ = server_->bind_to_any_port("127.0.0.1");
    if (port_ <= 0) {
        server_.reset();
        return false;
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    spdlog::info("DB MCP server listening on 127.0.0.1:{}", port_);
    return true;
}

void DbMcpServer::stop() {
    // release a parked connect_database handler, or the join below waits on it
    stopping_ = true;
    finishConnectRequest(false, "DearSQL closed the database tools.");

    if (server_) {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        server_.reset();
        port_ = 0;
        token_.clear();
    }
}

bool DbMcpServer::isRunning() const {
    return server_ != nullptr;
}

std::string DbMcpServer::url() const {
    return port_ > 0 ? std::format("http://127.0.0.1:{}/mcp", port_) : "";
}

void DbMcpServer::setNode(IDatabaseNode* node, const std::string& label) {
    std::lock_guard lock(nodeMutex_);
    node_ = node;
    nodeLabel_ = label;
}

json DbMcpServer::handleRpc(const json& req) {
    const std::string method = req.value("method", "");
    json response = {{"jsonrpc", "2.0"}, {"id", req.value("id", json{})}};

    if (method == "initialize") {
        response["result"] = {
            {"protocolVersion",
             req.value("params", json::object()).value("protocolVersion", "2025-03-26")},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "dearsql"}, {"version", "1.0"}}}};
    } else if (method == "tools/list") {
        response["result"] = {
            {"tools",
             json::array(
                 {{{"name", "list_databases"},
                   {"description",
                    "List the database connections saved in DearSQL and whether each one is "
                    "currently open."},
                   {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
                  {{"name", "connect_database"},
                   {"description",
                    "Open one of DearSQL's saved database connections by name, so its schema "
                    "can be inspected and queried. Use this when a database is listed as not "
                    "connected."},
                   {"inputSchema",
                    {{"type", "object"},
                     {"properties",
                      {{"name", {{"type", "string"}, {"description", "Connection name to open"}}}}},
                     {"required", json::array({"name"})}}}},
                  {{"name", "list_tables"},
                   {"description",
                    "List tables and views (with columns) of the database currently selected "
                    "in DearSQL."},
                   {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
                  {{"name", "run_query"},
                   {"description",
                    "Run a read-only SQL query (SELECT/SHOW/EXPLAIN/...) against the database "
                    "currently selected in DearSQL and return the rows."},
                   {"inputSchema",
                    {{"type", "object"},
                     {"properties",
                      {{"sql", {{"type", "string"}, {"description", "SQL to execute"}}}}},
                     {"required", json::array({"sql"})}}}}})}};
    } else if (method == "tools/call") {
        const json params = req.value("params", json::object());
        response["result"] =
            callTool(params.value("name", ""), params.value("arguments", json::object()));
    } else {
        response["error"] = {{"code", -32601}, {"message", "Method not found: " + method}};
    }
    return response;
}

void DbMcpServer::setConnections(std::vector<McpConnectionInfo> connections) {
    std::lock_guard lock(connectionsMutex_);
    connections_ = std::move(connections);
}

std::optional<std::string> DbMcpServer::takeConnectRequest() {
    std::lock_guard lock(connectMutex_);
    if (connectRequest_.empty() || connectTaken_) {
        return std::nullopt;
    }
    connectTaken_ = true;
    return connectRequest_;
}

void DbMcpServer::finishConnectRequest(bool ok, const std::string& message) {
    {
        std::lock_guard lock(connectMutex_);
        if (!connectBusy_) {
            return;
        }
        connectOk_ = ok;
        connectMessage_ = message;
        connectDone_ = true;
        connectRequest_.clear();
    }
    connectCv_.notify_all();
}

json DbMcpServer::callTool(const std::string& name, const json& args) {
    if (name == "list_databases") {
        std::lock_guard lock(connectionsMutex_);
        if (connections_.empty()) {
            return textResult("DearSQL has no saved database connections.");
        }
        std::string out = "Connections in DearSQL:\n";
        for (const auto& conn : connections_) {
            out += std::format("- {} ({}) - {}\n", conn.name, conn.type,
                               conn.connected ? "connected" : "not connected");
        }
        out += "\nUse connect_database to open one that is not connected.";
        return textResult(out);
    }

    if (name == "connect_database") {
        const std::string target = args.value("name", "");
        if (target.empty()) {
            return textResult("Missing 'name' argument.", true);
        }

        std::unique_lock lock(connectMutex_);
        if (stopping_) {
            return textResult("DearSQL closed the database tools.", true);
        }
        if (connectBusy_) {
            return textResult("Another connection attempt is already in progress.", true);
        }
        connectBusy_ = true;
        connectTaken_ = false;
        connectDone_ = false;
        connectRequest_ = target;

        const bool finished = connectCv_.wait_for(lock, std::chrono::seconds(60),
                                                  [this] { return connectDone_ || stopping_; });
        const bool ok = connectDone_ && connectOk_;
        const std::string message = connectDone_ ? connectMessage_
                                    : finished   ? "DearSQL closed the database tools."
                                                 : "Timed out waiting for the connection to open.";
        connectBusy_ = false;
        connectRequest_.clear();
        return textResult(message, !ok);
    }

    std::lock_guard lock(nodeMutex_);
    if (!node_) {
        return textResult("No database is open in DearSQL. Call list_databases to see the "
                          "saved connections, then connect_database to open one.",
                          true);
    }

    try {
        if (name == "list_tables") {
            std::string out = "Database: " + nodeLabel_ + "\n";
            if (!node_->isTablesLoaded()) {
                return textResult("Schema not loaded yet — the user needs to expand the "
                                  "database in the sidebar first.",
                                  true);
            }
            for (const auto& table : node_->getTables()) {
                out += "Table " + table.name + " (";
                for (size_t i = 0; i < table.columns.size(); ++i) {
                    if (i > 0) {
                        out += ", ";
                    }
                    out += table.columns[i].name + " " + table.columns[i].type;
                }
                out += ")\n";
            }
            if (node_->isViewsLoaded()) {
                for (const auto& view : node_->getViews()) {
                    out += "View " + view.name + "\n";
                }
            }
            return textResult(out);
        }

        if (name == "run_query") {
            const std::string sql = args.value("sql", "");
            if (sql.empty()) {
                return textResult("Missing 'sql' argument.", true);
            }
            if (!SqlGuard::isReadOnly(sql)) {
                return textResult(
                    "Rejected: only read-only statements (SELECT/SHOW/EXPLAIN/DESCRIBE/WITH) "
                    "are allowed through this tool.",
                    true);
            }
            // http thread; safe: sqlite is FULLMUTEX, duckdb has a mutex, servers pool
            const QueryResult result = node_->executeQuery(sql, MAX_ROWS + 1);
            return textResult(formatQueryResult(result), !result.success());
        }
    } catch (const std::exception& e) {
        return textResult(std::string("Tool failed: ") + e.what(), true);
    }

    return textResult("Unknown tool: " + name, true);
}
