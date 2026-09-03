#pragma once

#include "acp/client.hpp"
#include "acp/types.hpp"
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acp {

    struct SpawnOptions {
        std::string cwd;
        // added to (or replacing) the inherited environment
        std::vector<std::pair<std::string, std::string>> env;
        // inherited variables to drop, e.g. CLAUDECODE so claude does not think it is nested
        std::vector<std::string> dropEnv;
        // give the agent its own process group so stop() reaches grandchildren (npx → node)
        bool newProcessGroup = true;
    };

    using ResponseCallback = std::function<void(Response)>;

    // Client-side connection to one agent process: JSON-RPC 2.0 over the agent's
    // stdio. Requests return futures and can also take a callback, which runs on
    // the reader thread as soon as the response arrives.
    class Connection {
    public:
        // spawn `argv` and start talking to it. nullptr + error when it cannot be launched
        static std::pair<std::unique_ptr<Connection>, std::string>
        spawn(Client& client, const std::vector<std::string>& argv,
              const SpawnOptions& options = {});

        ~Connection();
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        // terminate the agent and join the reader; pending requests fail with an error
        void stop();
        [[nodiscard]] bool isRunning() const;

        // ---- agent methods (client → agent)
        std::future<Response> initialize(const ClientCapabilities& capabilities = {},
                                         int protocolVersion = kProtocolVersion,
                                         ResponseCallback callback = nullptr);
        std::future<Response> authenticate(const std::string& methodId,
                                           ResponseCallback callback = nullptr);
        std::future<Response> newSession(const std::string& cwd,
                                         const json& mcpServers = json::array(),
                                         ResponseCallback callback = nullptr);
        std::future<Response> loadSession(const std::string& sessionId, const std::string& cwd,
                                          const json& mcpServers = json::array(),
                                          ResponseCallback callback = nullptr);
        std::future<Response> prompt(const std::string& sessionId, const json& contentBlocks,
                                     ResponseCallback callback = nullptr);
        std::future<Response> setSessionMode(const std::string& sessionId,
                                             const std::string& modeId,
                                             ResponseCallback callback = nullptr);
        void cancel(const std::string& sessionId);

        // answer a PermissionRequest; empty optionId = cancelled
        void respondPermission(const json& rpcId, const std::string& optionId);

        // ---- raw JSON-RPC, for extension methods
        std::future<Response> request(const std::string& method, json params,
                                      ResponseCallback callback = nullptr);
        void notify(const std::string& method, json params);
        void respond(const json& id, json result);
        void respondError(const json& id, int code, const std::string& message);

    private:
        Connection() = default;

        void readerLoop();
        void stderrLoop();
        void handleMessage(const json& msg);
        void handleRequest(const json& msg);
        void handleResponse(const json& msg);
        void writeLine(const std::string& line);
        void failPending(const std::string& reason);

        struct Pending {
            std::promise<Response> promise;
            ResponseCallback callback;
        };

        Client* client_ = nullptr;
        long long pid_ = -1;
        int stdinFd_ = -1;
        int stdoutFd_ = -1;
        int stderrFd_ = -1;
        std::thread reader_;
        std::thread errReader_;

        std::mutex writeMutex_;
        std::mutex pendingMutex_;
        std::map<long long, Pending> pending_;
        long long nextId_ = 1;

        std::atomic<bool> exited_{false};
        std::atomic<bool> stopping_{false};
    };

} // namespace acp
