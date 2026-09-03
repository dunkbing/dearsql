#include "acp/connection.hpp"
#include "acp/log.hpp"

#include <algorithm>

namespace acp {

    namespace {
        Response closedResponse(const std::string& reason) {
            Response r;
            r.error = RpcError{-32000, reason, json()};
            return r;
        }
    } // namespace

    // ---------------------------------------------------------------- shared

    Connection::~Connection() {
        stop();
    }

    bool Connection::isRunning() const {
        return pid_ > 0 && !exited_;
    }

    std::future<Response> Connection::request(const std::string& method, json params,
                                              ResponseCallback callback) {
        std::promise<Response> promise;
        std::future<Response> future = promise.get_future();
        long long id = 0;
        {
            std::lock_guard lock(pendingMutex_);
            id = nextId_++;
            pending_[id] = Pending{std::move(promise), std::move(callback)};
        }
        if (!isRunning()) {
            failPending("agent is not running");
            return future;
        }
        writeLine(
            json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
        return future;
    }

    void Connection::notify(const std::string& method, json params) {
        writeLine(json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump());
    }

    void Connection::respond(const json& id, json result) {
        writeLine(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump());
    }

    void Connection::respondError(const json& id, int code, const std::string& message) {
        writeLine(
            json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}
                .dump());
    }

    std::future<Response> Connection::initialize(const ClientCapabilities& capabilities,
                                                 int protocolVersion, ResponseCallback callback) {
        return request(
            "initialize",
            {{"protocolVersion", protocolVersion}, {"clientCapabilities", capabilities.toJson()}},
            std::move(callback));
    }

    std::future<Response> Connection::authenticate(const std::string& methodId,
                                                   ResponseCallback callback) {
        return request("authenticate", {{"methodId", methodId}}, std::move(callback));
    }

    std::future<Response> Connection::newSession(const std::string& cwd, const json& mcpServers,
                                                 ResponseCallback callback) {
        return request("session/new", {{"cwd", cwd}, {"mcpServers", mcpServers}},
                       std::move(callback));
    }

    std::future<Response> Connection::loadSession(const std::string& sessionId,
                                                  const std::string& cwd, const json& mcpServers,
                                                  ResponseCallback callback) {
        return request("session/load",
                       {{"sessionId", sessionId}, {"cwd", cwd}, {"mcpServers", mcpServers}},
                       std::move(callback));
    }

    std::future<Response> Connection::prompt(const std::string& sessionId,
                                             const json& contentBlocks, ResponseCallback callback) {
        return request("session/prompt", {{"sessionId", sessionId}, {"prompt", contentBlocks}},
                       std::move(callback));
    }

    std::future<Response> Connection::setSessionMode(const std::string& sessionId,
                                                     const std::string& modeId,
                                                     ResponseCallback callback) {
        return request("session/set_mode", {{"sessionId", sessionId}, {"modeId", modeId}},
                       std::move(callback));
    }

    void Connection::cancel(const std::string& sessionId) {
        notify("session/cancel", {{"sessionId", sessionId}});
    }

    void Connection::respondPermission(const json& rpcId, const std::string& optionId) {
        const json outcome = optionId.empty()
                                 ? json{{"outcome", "cancelled"}}
                                 : json{{"outcome", "selected"}, {"optionId", optionId}};
        respond(rpcId, {{"outcome", outcome}});
    }

    void Connection::failPending(const std::string& reason) {
        std::map<long long, Pending> pending;
        {
            std::lock_guard lock(pendingMutex_);
            pending.swap(pending_);
        }
        for (auto& [id, p] : pending) {
            Response r = closedResponse(reason);
            if (p.callback) {
                p.callback(r);
            }
            p.promise.set_value(std::move(r));
        }
    }

    void Connection::handleMessage(const json& msg) {
        const bool hasMethod = msg.contains("method");
        const bool hasId = msg.contains("id");
        if (hasMethod && hasId) {
            handleRequest(msg);
        } else if (hasMethod) {
            const std::string method = msg["method"];
            const json params = msg.value("params", json::object());
            if (method == "session/update") {
                SessionNotification n;
                n.sessionId = params.value("sessionId", "");
                n.update = SessionUpdate::fromJson(params.value("update", json::object()));
                client_->sessionUpdate(n);
            } else {
                client_->extNotification(method, params);
            }
        } else if (hasId) {
            handleResponse(msg);
        }
    }

    void Connection::handleRequest(const json& msg) {
        const std::string method = msg["method"];
        const json params = msg.value("params", json::object());
        const json& id = msg["id"];

        if (method == "session/request_permission") {
            PermissionRequest req;
            req.rpcId = id;
            req.sessionId = params.value("sessionId", "");
            const json tc = params.value("toolCall", json::object());
            req.toolCall.id = tc.value("toolCallId", "");
            req.toolCall.title = tc.value("title", "");
            req.toolCall.kind = tc.value("kind", "");
            req.toolCall.status = tc.value("status", "");
            req.toolCall.content = tc.value("content", json::array());
            for (const auto& opt : params.value("options", json::array())) {
                req.options.push_back(
                    {opt.value("optionId", ""), opt.value("name", ""), opt.value("kind", "")});
            }
            client_->requestPermission(req);
            return;
        }

        std::optional<Response> reply;
        if (method == "fs/read_text_file") {
            reply = client_->readTextFile(params);
        } else if (method == "fs/write_text_file") {
            reply = client_->writeTextFile(params);
        } else {
            reply = client_->extRequest(method, params);
        }
        if (!reply) {
            respondError(id, -32601, "Method not supported: " + method);
        } else if (reply->error) {
            respondError(id, reply->error->code, reply->error->message);
        } else {
            respond(id, reply->result);
        }
    }

    void Connection::handleResponse(const json& msg) {
        Pending pending;
        bool found = false;
        {
            std::lock_guard lock(pendingMutex_);
            if (msg["id"].is_number_integer()) {
                auto it = pending_.find(msg["id"].get<long long>());
                if (it != pending_.end()) {
                    pending = std::move(it->second);
                    pending_.erase(it);
                    found = true;
                }
            }
        }
        if (!found) {
            log(LogLevel::Warn, "response for unknown request id: " + msg["id"].dump());
            return;
        }
        Response r;
        if (msg.contains("error")) {
            const json& err = msg["error"];
            r.error = RpcError{err.value("code", 0), err.value("message", "Agent error"),
                               err.value("data", json())};
        } else {
            r.result = msg.value("result", json::object());
        }
        if (pending.callback) {
            pending.callback(r);
        }
        pending.promise.set_value(std::move(r));
    }

} // namespace acp

#if defined(_WIN32)

// ponytail: no Windows transport yet (needs CreateProcess pipe plumbing)
namespace acp {
    std::pair<std::unique_ptr<Connection>, std::string>
    Connection::spawn(Client&, const std::vector<std::string>&, const SpawnOptions&) {
        return {nullptr, "ACP agents are not supported on Windows yet"};
    }
    void Connection::stop() {}
    void Connection::readerLoop() {}
    void Connection::stderrLoop() {}
    void Connection::writeLine(const std::string&) {}
} // namespace acp

#else

#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace acp {

    std::pair<std::unique_ptr<Connection>, std::string>
    Connection::spawn(Client& client, const std::vector<std::string>& argv,
                      const SpawnOptions& options) {
        if (argv.empty()) {
            return {nullptr, "No agent command configured"};
        }

        signal(SIGPIPE, SIG_IGN); // dead agent stdin → EPIPE, not process exit

        int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
        if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
            for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
                if (fd >= 0) {
                    close(fd);
                }
            }
            return {nullptr, "Failed to create pipes"};
        }

        std::vector<const char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) {
            cargv.push_back(a.c_str());
        }
        cargv.push_back(nullptr);

        std::vector<std::string> envStrings;
        for (char** e = environ; *e; ++e) {
            std::string var(*e);
            const std::string name = var.substr(0, var.find('='));
            const bool overridden = std::any_of(options.env.begin(), options.env.end(),
                                                [&](const auto& kv) { return kv.first == name; });
            const bool dropped = std::find(options.dropEnv.begin(), options.dropEnv.end(), name) !=
                                 options.dropEnv.end();
            if (!overridden && !dropped) {
                envStrings.push_back(std::move(var));
            }
        }
        for (const auto& [name, value] : options.env) {
            envStrings.push_back(name + "=" + value);
        }
        std::vector<const char*> envp;
        for (const auto& s : envStrings) {
            envp.push_back(s.c_str());
        }
        envp.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, inPipe[1]);
        posix_spawn_file_actions_addclose(&actions, outPipe[0]);
        posix_spawn_file_actions_addclose(&actions, errPipe[0]);
        if (!options.cwd.empty()) {
            posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
        }

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        if (options.newProcessGroup) {
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
            posix_spawnattr_setpgroup(&attr, 0);
        }

        pid_t pid = -1;
        const int rc =
            posix_spawnp(&pid, cargv[0], &actions, &attr, const_cast<char* const*>(cargv.data()),
                         const_cast<char* const*>(envp.data()));
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);

        close(inPipe[0]);
        close(outPipe[1]);
        close(errPipe[1]);

        if (rc != 0) {
            close(inPipe[1]);
            close(outPipe[0]);
            close(errPipe[0]);
            return {nullptr, std::string("Failed to launch agent: ") + std::strerror(rc)};
        }

        std::unique_ptr<Connection> conn(new Connection());
        conn->client_ = &client;
        conn->pid_ = pid;
        conn->stdinFd_ = inPipe[1];
        conn->stdoutFd_ = outPipe[0];
        conn->stderrFd_ = errPipe[0];
        Connection* raw = conn.get();
        conn->reader_ = std::thread([raw] { raw->readerLoop(); });
        conn->errReader_ = std::thread([raw] { raw->stderrLoop(); });
        log(LogLevel::Info, "agent spawned (pid " + std::to_string(pid) + "): " + argv[0]);
        return {std::move(conn), ""};
    }

    void Connection::stop() {
        stopping_ = true; // lets the reader threads fall out of poll()

        if (pid_ > 0) {
            const auto pid = static_cast<pid_t>(pid_);
            kill(-pid, SIGTERM); // whole group, so the node grandchild goes too
            kill(pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid, &status, WNOHANG) != 0) {
                    break;
                }
                usleep(50 * 1000);
            }
            if (kill(pid, 0) == 0) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            log(LogLevel::Info, "agent stopped (pid " + std::to_string(pid_) + ")");
            pid_ = -1;
        }
        // reader writes to stdin too; join before closing the fd
        if (reader_.joinable()) {
            reader_.join();
        }
        if (errReader_.joinable()) {
            errReader_.join();
        }
        {
            std::lock_guard lock(writeMutex_);
            if (stdinFd_ >= 0) {
                close(stdinFd_);
                stdinFd_ = -1;
            }
        }
        if (stdoutFd_ >= 0) {
            close(stdoutFd_);
            stdoutFd_ = -1;
        }
        if (stderrFd_ >= 0) {
            close(stderrFd_);
            stderrFd_ = -1;
        }
        failPending("connection closed");
    }

    void Connection::writeLine(const std::string& line) {
        const std::string data = line + "\n";
        std::lock_guard lock(writeMutex_);
        if (stdinFd_ < 0) {
            return;
        }
        size_t written = 0;
        while (written < data.size()) {
            const ssize_t n = write(stdinFd_, data.data() + written, data.size() - written);
            if (n <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                log(LogLevel::Warn, std::string("write to agent failed: ") + std::strerror(errno));
                return;
            }
            written += static_cast<size_t>(n);
        }
    }

    namespace {
        // poll + read fd line by line until it closes or `stopping` flips
        template <typename OnLine>
        void pumpLines(int fd, const std::atomic<bool>& stopping, OnLine onLine) {
            std::string buffer;
            char chunk[8192];
            while (!stopping) {
                // poll rather than block in read(): anything else that inherited the
                // pipe would keep read() from ever returning, and stop() would hang
                pollfd pfd{fd, POLLIN, 0};
                const int ready = poll(&pfd, 1, 100);
                if (ready == 0) {
                    continue;
                }
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                const ssize_t n = read(fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    break;
                }
                buffer.append(chunk, static_cast<size_t>(n));
                size_t pos = 0;
                while (true) {
                    const auto nl = buffer.find('\n', pos);
                    if (nl == std::string::npos) {
                        break;
                    }
                    std::string line = buffer.substr(pos, nl - pos);
                    pos = nl + 1;
                    if (!line.empty()) {
                        onLine(line);
                    }
                }
                buffer.erase(0, pos);
            }
        }
    } // namespace

    void Connection::readerLoop() {
        pumpLines(stdoutFd_, stopping_, [this](const std::string& line) {
            try {
                handleMessage(json::parse(line));
            } catch (const std::exception& e) {
                log(LogLevel::Warn, std::string("bad message: ") + e.what() + " (" +
                                        line.substr(0, std::min<size_t>(line.size(), 200)) + ")");
            }
        });
        exited_ = true;
        failPending("agent exited");
        if (!stopping_) { // a stop we asked for is not an agent crash
            client_->agentExited();
        }
    }

    void Connection::stderrLoop() {
        pumpLines(stderrFd_, stopping_,
                  [this](const std::string& line) { client_->agentStderr(line); });
    }

} // namespace acp

#endif
