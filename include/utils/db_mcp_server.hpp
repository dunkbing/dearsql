#pragma once

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

class IDatabaseNode;
namespace httplib {
    class Server;
}

// Minimal MCP server (streamable HTTP) exposing read-only query tools over the
// currently selected database node. Handed to ACP agents via session/new so
// they can inspect schema and run SELECTs themselves.
class DbMcpServer {
public:
    DbMcpServer();
    ~DbMcpServer();

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] std::string url() const; // http://127.0.0.1:<port>/mcp

    // node may be null (no database selected); label names it in tool output.
    // ponytail: raw pointer, cleared by the panel on selection change; a refresh
    // mid-query can still dangle it, same exposure as editor tabs
    void setNode(IDatabaseNode* node, const std::string& label);

private:
    nlohmann::json handleRpc(const nlohmann::json& req);
    nlohmann::json callTool(const std::string& name, const nlohmann::json& args);

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    int port_ = 0;

    mutable std::mutex nodeMutex_;
    IDatabaseNode* node_ = nullptr;
    std::string nodeLabel_;
};
