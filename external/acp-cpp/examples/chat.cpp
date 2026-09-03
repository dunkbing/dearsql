// Minimal terminal client: spawn an agent, open a session, chat line by line.
//   acp-chat claude-agent-acp
//   acp-chat npx --yes @agentclientprotocol/claude-agent-acp
#include <acp/connection.hpp>
#include <acp/log.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
    class Terminal : public acp::Client {
    public:
        acp::Connection* conn = nullptr;

        void sessionUpdate(const acp::SessionNotification& n) override {
            using K = acp::SessionUpdate::Kind;
            switch (n.update.kind) {
            case K::AgentMessageChunk:
                std::cout << n.update.text << std::flush;
                break;
            case K::AgentThoughtChunk:
                std::cerr << "\x1b[2m" << n.update.text << "\x1b[0m" << std::flush;
                break;
            case K::ToolCall:
                std::cout << "\n[tool] " << n.update.toolCall.title << "\n";
                break;
            case K::ToolCallUpdate:
                if (!n.update.toolCall.status.empty()) {
                    std::cout << "[tool " << n.update.toolCall.status << "]\n";
                }
                break;
            default:
                break;
            }
        }

        // auto-approve: pick the first allow_* option
        void requestPermission(const acp::PermissionRequest& req) override {
            std::string choice;
            for (const auto& opt : req.options) {
                if (opt.kind.rfind("allow", 0) == 0) {
                    choice = opt.optionId;
                    break;
                }
            }
            std::cout << "\n[permission] " << req.toolCall.title << " -> "
                      << (choice.empty() ? "cancelled" : choice) << "\n";
            conn->respondPermission(req.rpcId, choice);
        }

        void agentStderr(const std::string& line) override {
            std::cerr << "[agent] " << line << "\n";
        }
        void agentExited() override {
            std::cerr << "[agent exited]\n";
            exited = true;
        }

        std::atomic<bool> exited{false};
    };
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: acp-chat <agent command...>\n";
        return 2;
    }
    acp::setLogger([](acp::LogLevel, const std::string& m) { std::cerr << "[acp] " << m << "\n"; });

    Terminal terminal;
    acp::SpawnOptions opts;
    opts.cwd = std::filesystem::current_path().string();
    auto [conn, err] = acp::Connection::spawn(terminal, {argv + 1, argv + argc}, opts);
    if (!conn) {
        std::cerr << err << "\n";
        return 1;
    }
    terminal.conn = conn.get();

    auto init = conn->initialize().get();
    if (!init.ok()) {
        std::cerr << "initialize failed: " << init.error->describe() << "\n";
        return 1;
    }
    auto session = conn->newSession(opts.cwd).get();
    if (!session.ok()) {
        std::cerr << "session/new failed: " << session.error->describe() << "\n";
        return 1;
    }
    const std::string sessionId = session.result.value("sessionId", "");

    std::string line;
    std::cout << "> " << std::flush;
    while (!terminal.exited && std::getline(std::cin, line)) {
        if (line.empty()) {
            std::cout << "> " << std::flush;
            continue;
        }
        auto turn = conn->prompt(sessionId, acp::json::array({acp::textBlock(line)})).get();
        if (!turn.ok()) {
            std::cerr << "\nprompt failed: " << turn.error->describe() << "\n";
        }
        std::cout << "\n> " << std::flush;
    }
    conn->stop();
    return 0;
}
