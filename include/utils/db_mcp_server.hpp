#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class IDatabaseNode;
namespace httplib {
    class Server;
}

// Minimal MCP server (streamable HTTP) exposing read-only query tools over the
// currently selected database node. Handed to ACP agents via session/new so
// they can inspect schema and run SELECTs themselves.
struct McpConnectionInfo {
    std::string name;
    std::string type;
    bool connected = false;
};

class DbMcpServer {
public:
    DbMcpServer();
    ~DbMcpServer();

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] std::string url() const; // http://127.0.0.1:<port>/mcp

    // bearer token required on every request, regenerated per start(). the socket is
    // loopback-only but any local process could otherwise reach the database tools.
    [[nodiscard]] std::string token() const {
        return token_;
    }

    // node may be null (no database selected); label names it in tool output.
    // ponytail: raw pointer, cleared by the panel on selection change; a refresh
    // mid-query can still dangle it, same exposure as editor tabs
    void setNode(IDatabaseNode* node, const std::string& label);

    // every saved connection, so the agent can see ones that are not open yet
    void setConnections(std::vector<McpConnectionInfo> connections);

    // connect_database is answered by the UI thread: opening a connection touches
    // app state and must not happen on the http listener thread. the handler blocks
    // until finishConnectRequest() reports the outcome.
    std::optional<std::string> takeConnectRequest();
    void finishConnectRequest(bool ok, const std::string& message);

private:
    nlohmann::json handleRpc(const nlohmann::json& req);
    nlohmann::json callTool(const std::string& name, const nlohmann::json& args);

    std::unique_ptr<httplib::Server> server_;
    std::string token_;
    std::thread thread_;
    int port_ = 0;
    std::atomic<bool> stopping_ = false; // connect_database fails fast during stop()

    mutable std::mutex nodeMutex_;
    IDatabaseNode* node_ = nullptr;
    std::string nodeLabel_;

    mutable std::mutex connectionsMutex_;
    std::vector<McpConnectionInfo> connections_;

    std::mutex connectMutex_;
    std::condition_variable connectCv_;
    std::string connectRequest_; // set by the tool, consumed by the UI thread
    bool connectTaken_ = false;
    bool connectDone_ = false;
    bool connectOk_ = false;
    bool connectBusy_ = false;
    std::string connectMessage_;
};
