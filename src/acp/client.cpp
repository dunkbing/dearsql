#include "acp/client.hpp"
#include "acp/agents.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

#if defined(_WIN32)

// ponytail: no Windows ACP yet (needs CreateProcess pipe plumbing, same split as ssh tunnel)
AcpClient::~AcpClient() = default;
std::pair<bool, std::string> AcpClient::start(const std::vector<std::string>&, const std::string&,
                                              const std::string&, const std::string&,
                                              const std::string&) {
    return {false, "AI agents via ACP are not supported on Windows yet"};
}
void AcpClient::stop() {}
bool AcpClient::isRunning() const {
    return false;
}
bool AcpClient::isSessionReady() const {
    return false;
}
bool AcpClient::isTurnActive() const {
    return false;
}
void AcpClient::prompt(const nlohmann::json&) {}
void AcpClient::cancelTurn() {}
void AcpClient::respondPermission(const nlohmann::json&, const std::string&) {}
std::vector<AcpEvent> AcpClient::drainEvents() {
    return {};
}
void AcpClient::readerLoop() {}
void AcpClient::stderrLoop() {}
void AcpClient::handleMessage(const nlohmann::json&) {}
void AcpClient::handleResponse(const nlohmann::json&) {}
void AcpClient::handleSessionUpdate(const nlohmann::json&) {}
void AcpClient::sendMessage(const nlohmann::json&) {}
long long AcpClient::sendRequest(const std::string&, const nlohmann::json&) {
    return 0;
}
void AcpClient::pushEvent(AcpEvent) {}

#else

#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
    constexpr int ACP_PROTOCOL_VERSION = 1;

    // best-effort text from an ACP content block
    std::string contentBlockText(const json& block) {
        if (!block.is_object()) {
            return "";
        }
        const std::string type = block.value("type", "");
        if (type == "text") {
            return block.value("text", "");
        }
        if (type == "resource") {
            return block.value("resource", json::object()).value("text", "");
        }
        if (type == "resource_link") {
            return block.value("uri", "");
        }
        return "";
    }

    std::string toolCallContentText(const json& contentArr) {
        std::string out;
        if (!contentArr.is_array()) {
            return out;
        }
        for (const auto& item : contentArr) {
            const std::string type = item.value("type", "");
            if (type == "content") {
                out += contentBlockText(item.value("content", json::object()));
            } else if (type == "diff") {
                out += "diff: " + item.value("path", "");
            } else if (type == "terminal") {
                out += "[terminal output]";
            }
            if (!out.empty() && out.back() != '\n') {
                out += "\n";
            }
        }
        return out;
    }
} // namespace

AcpClient::~AcpClient() {
    stop();
}

std::pair<bool, std::string> AcpClient::start(const std::vector<std::string>& argv,
                                              const std::string& cwd, const std::string& mcpUrl,
                                              const std::string& mcpName,
                                              const std::string& mcpToken) {
    if (pid_ > 0) {
        return {true, ""};
    }
    if (argv.empty()) {
        return {false, "No agent command configured"};
    }

    stopping_ = false;
    cwd_ = cwd;
    mcpUrl_ = mcpUrl;
    mcpName_ = mcpName;
    mcpToken_ = mcpToken;

    signal(SIGPIPE, SIG_IGN); // dead agent stdin → EPIPE, not app exit

    int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
        return {false, "Failed to create pipes"};
    }

    std::vector<const char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        cargv.push_back(a.c_str());
    }
    cargv.push_back(nullptr);

    // GUI apps on macOS get a minimal PATH; use the login shell's so agents
    // installed via npm/homebrew are found
    std::vector<std::string> envStrings;
    std::vector<const char*> envp;
    const std::string shellPath = AcpAgents::loginShellPath();
    for (char** e = environ; *e; ++e) {
        std::string var(*e);
        if (!shellPath.empty() && var.starts_with("PATH=")) {
            continue;
        }
        // claude refuses to start when it thinks it is nested in another
        // session; inherited when DearSQL itself was launched from one
        if (var.starts_with("CLAUDECODE=") || var.starts_with("CLAUDE_CODE_ENTRYPOINT=")) {
            continue;
        }
        envStrings.push_back(std::move(var));
    }
    if (!shellPath.empty()) {
        envStrings.push_back("PATH=" + shellPath);
    }
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
    if (!cwd.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    }

    // give the agent its own process group: `npx` spawns node as a grandchild, and
    // signalling only the direct child leaves node alive holding the pipes open
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

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
        return {false, std::string("Failed to launch agent: ") + std::strerror(rc)};
    }

    pid_ = pid;
    stdinFd_ = inPipe[1];
    stdoutFd_ = outPipe[0];
    stderrFd_ = errPipe[0];
    exited_ = false;

    reader_ = std::thread([this] { readerLoop(); });
    errReader_ = std::thread([this] { stderrLoop(); });

    sendRequest("initialize", {{"protocolVersion", ACP_PROTOCOL_VERSION},
                               {"clientCapabilities",
                                {{"fs", {{"readTextFile", false}, {"writeTextFile", false}}},
                                 {"terminal", false}}}});

    spdlog::info("ACP agent spawned (pid {}): {}", pid_, argv[0]);
    return {true, ""};
}

void AcpClient::stop() {
    stopping_ = true; // lets the reader threads fall out of poll()

    if (pid_ > 0) {
        const auto pid = static_cast<pid_t>(pid_);
        // negative pid signals the whole group, so the node grandchild goes too
        kill(-pid, SIGTERM);
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
        spdlog::info("ACP agent stopped (pid {})", pid_);
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
    sessionReady_ = false;
    turnActive_ = false;
}

bool AcpClient::isRunning() const {
    return pid_ > 0 && !exited_;
}

bool AcpClient::isSessionReady() const {
    return sessionReady_ && isRunning();
}

bool AcpClient::isTurnActive() const {
    return turnActive_ && isRunning();
}

void AcpClient::prompt(const json& contentBlocks) {
    if (!isSessionReady() || turnActive_) {
        return;
    }
    turnActive_ = true;
    sendRequest("session/prompt", {{"sessionId", sessionId_}, {"prompt", contentBlocks}});
}

void AcpClient::cancelTurn() {
    if (!isSessionReady()) {
        return;
    }
    sendMessage({{"jsonrpc", "2.0"},
                 {"method", "session/cancel"},
                 {"params", {{"sessionId", sessionId_}}}});
}

void AcpClient::respondPermission(const json& rpcId, const std::string& optionId) {
    json outcome = optionId.empty() ? json{{"outcome", "cancelled"}}
                                    : json{{"outcome", "selected"}, {"optionId", optionId}};
    sendMessage({{"jsonrpc", "2.0"}, {"id", rpcId}, {"result", {{"outcome", outcome}}}});
}

std::vector<AcpEvent> AcpClient::drainEvents() {
    std::lock_guard lock(stateMutex_);
    std::vector<AcpEvent> out;
    out.swap(events_);
    return out;
}

void AcpClient::readerLoop() {
    std::string buffer;
    char chunk[8192];
    while (!stopping_) {
        // poll rather than block in read(): anything else that inherited the pipe
        // would keep read() from ever returning, and stop() would hang on join()
        pollfd pfd{stdoutFd_, POLLIN, 0};
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
        const ssize_t n = read(stdoutFd_, chunk, sizeof(chunk));
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
            if (line.empty()) {
                continue;
            }
            try {
                handleMessage(json::parse(line));
            } catch (const std::exception& e) {
                spdlog::warn("ACP: bad message: {} ({})", e.what(),
                             line.substr(0, std::min<size_t>(line.size(), 200)));
            }
        }
        buffer.erase(0, pos);
    }
    exited_ = true;
    sessionReady_ = false;
    turnActive_ = false;
    if (!stopping_) { // a stop we asked for is not an agent crash
        pushEvent({.type = AcpEvent::Type::Exited});
    }
}

void AcpClient::stderrLoop() {
    std::string buffer;
    char chunk[4096];
    while (!stopping_) {
        pollfd pfd{stderrFd_, POLLIN, 0};
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
        const ssize_t n = read(stderrFd_, chunk, sizeof(chunk));
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
                spdlog::debug("ACP agent stderr: {}", line);
            }
        }
        buffer.erase(0, pos);
    }
}

void AcpClient::handleMessage(const json& msg) {
    const bool hasMethod = msg.contains("method");
    const bool hasId = msg.contains("id");

    if (hasMethod && hasId) {
        // request from agent
        const std::string method = msg["method"];
        const json params = msg.value("params", json::object());
        if (method == "session/request_permission") {
            AcpEvent ev;
            ev.type = AcpEvent::Type::Permission;
            ev.permission.rpcId = msg["id"];
            ev.permission.title =
                params.value("toolCall", json::object()).value("title", "Agent action");
            for (const auto& opt : params.value("options", json::array())) {
                ev.permission.options.push_back(
                    {opt.value("optionId", ""), opt.value("name", ""), opt.value("kind", "")});
            }
            pushEvent(std::move(ev));
        } else {
            sendMessage(
                {{"jsonrpc", "2.0"},
                 {"id", msg["id"]},
                 {"error", {{"code", -32601}, {"message", "Method not supported: " + method}}}});
        }
        return;
    }

    if (hasMethod) {
        // notification
        if (msg["method"] == "session/update") {
            handleSessionUpdate(
                msg.value("params", json::object()).value("update", json::object()));
        }
        return;
    }

    if (hasId) {
        handleResponse(msg);
    }
}

void AcpClient::handleResponse(const json& msg) {
    std::string method;
    {
        std::lock_guard lock(stateMutex_);
        if (msg["id"].is_number_integer()) {
            auto it = pending_.find(msg["id"].get<long long>());
            if (it != pending_.end()) {
                method = it->second;
                pending_.erase(it);
            }
        }
    }

    if (msg.contains("error")) {
        const auto& err = msg["error"];
        std::string text = err.value("message", "Agent error");
        if (err.contains("data") && err["data"].is_object() && err["data"].contains("details")) {
            text += ": " + err["data"]["details"].dump();
        }
        if (method == "session/prompt") {
            turnActive_ = false;
        }
        pushEvent({.type = AcpEvent::Type::Error, .text = text});
        return;
    }

    const json result = msg.value("result", json::object());
    if (method == "initialize") {
        const bool httpMcp = result.value("agentCapabilities", json::object())
                                 .value("mcpCapabilities", json::object())
                                 .value("http", false);
        json mcpServers = json::array();
        if (!mcpUrl_.empty() && httpMcp) {
            // ACP McpServerHttp: headers is an array of {name, value}
            json headers = json::array();
            if (!mcpToken_.empty()) {
                headers.push_back({{"name", "Authorization"}, {"value", "Bearer " + mcpToken_}});
            }
            mcpServers.push_back(
                {{"type", "http"}, {"name", mcpName_}, {"url", mcpUrl_}, {"headers", headers}});
        }
        sendRequest("session/new", {{"cwd", cwd_}, {"mcpServers", mcpServers}});
    } else if (method == "session/new") {
        {
            std::lock_guard lock(stateMutex_);
            sessionId_ = result.value("sessionId", "");
        }
        sessionReady_ = true;
        pushEvent({.type = AcpEvent::Type::SessionReady});
    } else if (method == "session/prompt") {
        turnActive_ = false;
        pushEvent({.type = AcpEvent::Type::TurnEnded, .text = result.value("stopReason", "")});
    }
}

void AcpClient::handleSessionUpdate(const json& update) {
    const std::string kind = update.value("sessionUpdate", "");

    if (kind == "agent_message_chunk" || kind == "agent_thought_chunk") {
        AcpEvent ev;
        ev.type = kind == "agent_message_chunk" ? AcpEvent::Type::MessageDelta
                                                : AcpEvent::Type::ThoughtDelta;
        ev.text = contentBlockText(update.value("content", json::object()));
        if (!ev.text.empty()) {
            pushEvent(std::move(ev));
        }
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        AcpEvent ev;
        ev.type = kind == "tool_call" ? AcpEvent::Type::ToolCall : AcpEvent::Type::ToolCallUpdate;
        ev.tool.id = update.value("toolCallId", "");
        ev.tool.title = update.value("title", "");
        ev.tool.kind = update.value("kind", "");
        ev.tool.status = update.value("status", "");
        ev.tool.output = toolCallContentText(update.value("content", json::array()));
        pushEvent(std::move(ev));
    } else if (kind == "plan") {
        AcpEvent ev;
        ev.type = AcpEvent::Type::Plan;
        for (const auto& e : update.value("entries", json::array())) {
            ev.plan.push_back(
                {e.value("content", ""), e.value("priority", ""), e.value("status", "")});
        }
        pushEvent(std::move(ev));
    } else if (kind == "available_commands_update") {
        AcpEvent ev;
        ev.type = AcpEvent::Type::Commands;
        for (const auto& cmd : update.value("availableCommands", json::array())) {
            AcpCommand out;
            out.name = cmd.value("name", "");
            out.description = cmd.value("description", "");
            // AvailableCommandInput is currently only the unstructured {hint} form
            if (cmd.contains("input") && cmd["input"].is_object()) {
                out.hint = cmd["input"].value("hint", "");
            }
            if (!out.name.empty()) {
                ev.commands.push_back(std::move(out));
            }
        }
        pushEvent(std::move(ev));
    }
    // user_message_chunk / current_mode_update: ignored
}

void AcpClient::sendMessage(const json& msg) {
    const std::string line = msg.dump() + "\n";
    std::lock_guard lock(writeMutex_);
    if (stdinFd_ < 0) {
        return;
    }
    size_t written = 0;
    while (written < line.size()) {
        const ssize_t n = write(stdinFd_, line.data() + written, line.size() - written);
        if (n <= 0) {
            if (errno == EINTR) {
                continue;
            }
            spdlog::warn("ACP: write to agent failed: {}", std::strerror(errno));
            return;
        }
        written += static_cast<size_t>(n);
    }
}

long long AcpClient::sendRequest(const std::string& method, const json& params) {
    long long id = 0;
    {
        std::lock_guard lock(stateMutex_);
        id = nextId_++;
        pending_[id] = method;
    }
    sendMessage({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    return id;
}

void AcpClient::pushEvent(AcpEvent ev) {
    std::lock_guard lock(stateMutex_);
    events_.push_back(std::move(ev));
}

#endif
