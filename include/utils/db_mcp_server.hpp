#pragma once

#include "database/async_helper.hpp"
#include "mcp/db_tools.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
    class Server;
}

// serves the agent database tools (mcp::Server) over loopback streamable http.
// handed to ACP agents in session/new. the panel publishes connections and the
// current selection every frame; opening a connection is done by the UI thread.
struct McpConnectionInfo {
    std::string name;
    std::string type;
    bool connected = false;
    std::shared_ptr<dearsql::IConnection> lib; // null when closed
};

class DbMcpServer : private mcp::Host {
public:
    DbMcpServer();
    ~DbMcpServer() override;

    bool start();
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] std::string url() const; // http://127.0.0.1:<port>/mcp

    // bearer token required on every request, regenerated per start(). the socket is
    // loopback-only but any local process could otherwise reach the database tools.
    [[nodiscard]] std::string token() const {
        return token_;
    }

    // every saved connection, so the agent can see ones that are not open yet
    void setConnections(std::vector<McpConnectionInfo> connections);
    // what the user has selected: connection name and database ("db.schema" allowed)
    void setFocus(const std::string& connection, const std::string& database);

    // connect is answered by the UI thread: opening a connection touches app state
    // and must not happen on the http listener thread. the handler blocks until
    // finishConnectRequest() reports the outcome.
    std::optional<std::string> takeConnectRequest();
    // name: the connection the result is for; a late answer for an earlier,
    // timed-out request is ignored. empty = whatever is pending (shutdown)
    void finishConnectRequest(bool ok, const std::string& message, const std::string& name = "");

private:
    // mcp::Host
    std::vector<mcp::ConnectionEntry> connections() override;
    std::string connect(const std::string& name) override;
    std::shared_ptr<dearsql::IConnection> connection(const std::string& name) override;
    std::pair<std::string, std::string> focus() override;

    mcp::Server tools_{*this};
    std::unique_ptr<httplib::Server> server_;
    std::string token_;
    AsyncOperation<bool> listenOp_;
    int port_ = 0;
    std::atomic<bool> stopping_ = false; // connect fails fast during stop()

    mutable std::mutex stateMutex_;
    std::vector<McpConnectionInfo> connections_;
    std::pair<std::string, std::string> focus_;

    std::mutex connectMutex_;
    std::condition_variable connectCv_;
    std::string connectRequest_; // set by the tool, consumed by the UI thread
    bool connectTaken_ = false;
    bool connectDone_ = false;
    bool connectOk_ = false;
    bool connectBusy_ = false;
    std::string connectMessage_;
};
