#include "utils/db_mcp_server.hpp"

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
        for (int i = 0; i < 32; ++i)
            token += HEX[dist(rd)];
        return token;
    }
} // namespace

DbMcpServer::DbMcpServer() = default;

DbMcpServer::~DbMcpServer() {
    stop();
}

bool DbMcpServer::start() {
    if (server_)
        return true;
    server_ = std::make_unique<httplib::Server>();
    token_ = makeToken();
    stopping_ = false;
    tools_.resume();

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
        const json reply = tools_.handle(rpc);
        if (reply.is_null()) {
            res.status = 202; // notification
            return;
        }
        res.set_content(reply.dump(), "application/json");
    });

    port_ = server_->bind_to_any_port("127.0.0.1");
    if (port_ <= 0) {
        server_.reset();
        return false;
    }
    listenOp_.setWakesFrames(false);
    listenOp_.check();
    listenOp_.start([this] { return server_->listen_after_bind(); });
    spdlog::info("DB MCP server listening on 127.0.0.1:{}", port_);
    return true;
}

void DbMcpServer::stop() {
    // release a parked connect handler, or the join below waits on it
    stopping_ = true;
    finishConnectRequest(false, "DearSQL closed the database tools.");

    if (server_) {
        // the join below waits for in-flight handlers: stop their query server-side
        // (ponytail: a backend that cannot cancel, Mongo/Redis, still runs it out)
        tools_.interrupt();
        server_->stop();
        listenOp_.wait();
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

void DbMcpServer::setConnections(std::vector<McpConnectionInfo> connections) {
    std::lock_guard lock(stateMutex_);
    connections_ = std::move(connections);
}

void DbMcpServer::setFocus(const std::string& connection, const std::string& database) {
    std::lock_guard lock(stateMutex_);
    focus_ = {connection, database};
}

std::vector<mcp::ConnectionEntry> DbMcpServer::connections() {
    std::lock_guard lock(stateMutex_);
    std::vector<mcp::ConnectionEntry> out;
    for (const auto& c : connections_)
        out.push_back({c.name, c.type, c.connected && c.lib});
    return out;
}

std::shared_ptr<dearsql::IConnection> DbMcpServer::connection(const std::string& name) {
    std::lock_guard lock(stateMutex_);
    for (const auto& c : connections_) {
        if (c.name == name)
            return c.lib;
    }
    return nullptr;
}

std::pair<std::string, std::string> DbMcpServer::focus() {
    std::lock_guard lock(stateMutex_);
    return focus_;
}

std::optional<std::string> DbMcpServer::takeConnectRequest() {
    std::lock_guard lock(connectMutex_);
    if (connectRequest_.empty() || connectTaken_)
        return std::nullopt;
    connectTaken_ = true;
    return connectRequest_;
}

void DbMcpServer::finishConnectRequest(bool ok, const std::string& message,
                                       const std::string& name) {
    {
        std::lock_guard lock(connectMutex_);
        if (!connectBusy_ || (!name.empty() && name != connectRequest_))
            return;
        connectOk_ = ok;
        connectMessage_ = message;
        connectDone_ = true;
        connectRequest_.clear();
    }
    connectCv_.notify_all();
}

// runs on the http thread: hand the request to the UI thread and wait for it
std::string DbMcpServer::connect(const std::string& name) {
    std::unique_lock lock(connectMutex_);
    if (stopping_)
        return "DearSQL closed the database tools.";
    if (connectBusy_)
        return "another connection attempt is in progress";
    connectBusy_ = true;
    connectTaken_ = false;
    connectDone_ = false;
    connectRequest_ = name;

    const bool finished = connectCv_.wait_for(lock, std::chrono::seconds(60),
                                              [this] { return connectDone_ || stopping_; });
    const bool ok = connectDone_ && connectOk_;
    const std::string message = connectDone_ ? connectMessage_
                                : finished   ? "DearSQL closed the database tools."
                                             : "timed out waiting for the connection to open";
    connectBusy_ = false;
    connectRequest_.clear();
    if (!ok)
        return message;
    // the panel republishes connections next frame; until then nothing is open yet
    lock.unlock();
    for (int i = 0; i < 50 && !this->connection(name); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return "";
}
