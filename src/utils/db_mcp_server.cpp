#include "utils/db_mcp_server.hpp"

#include "database/database_node.hpp"
#include "utils/sql_guard.hpp"
#include <format>
#include <spdlog/spdlog.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

using json = nlohmann::json;

namespace {
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

    server_->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
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
    if (server_) {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        server_.reset();
        port_ = 0;
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
                 {{{"name", "list_tables"},
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

json DbMcpServer::callTool(const std::string& name, const json& args) {
    std::lock_guard lock(nodeMutex_);
    if (!node_) {
        return textResult("No database is selected in DearSQL.", true);
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
            const QueryResult result = node_->executeQuery(sql, MAX_ROWS + 1);
            return textResult(formatQueryResult(result), !result.success());
        }
    } catch (const std::exception& e) {
        return textResult(std::string("Tool failed: ") + e.what(), true);
    }

    return textResult("Unknown tool: " + name, true);
}
