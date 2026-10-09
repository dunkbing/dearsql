// the app's terminal modes: `dearsql --tui` (a terminal client over libdearsql)
// and `dearsql --mcp` (a stdio MCP server with the GUI's agent database tools)
#include "cli/cli.hpp"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include "cli/connections.hpp"
#include "cli/tui.hpp"
#include "mcp/db_tools.hpp"
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

    constexpr const char* USAGE = R"(usage:
  dearsql [file]                    open the app (and the file, if given)
  dearsql --tui [connection]        browse and query in the terminal
  dearsql --mcp [connection...]     MCP server on stdin/stdout for coding agents

connection: a saved DearSQL connection name, a URL (postgres://user:pass@host/db,
mysql://..., mongodb://..., redis://..., mssql://..., oracle://...), or a
.sqlite/.duckdb/.csv file. Without one, every saved connection is listed.

register with an agent, e.g.:
  claude mcp add dearsql -- dearsql --mcp
  codex mcp add dearsql -- dearsql --mcp
)";

    // the agent's view of the cli's connections; nothing is "selected", so a
    // single connection given on the command line becomes the focus
    class StdioHost : public mcp::Host {
    public:
        StdioHost(CliConnections& conns, std::string focus)
            : conns_(conns), focus_(std::move(focus)) {}

        std::vector<mcp::ConnectionEntry> connections() override {
            std::vector<mcp::ConnectionEntry> out;
            for (auto* e : conns_.entries())
                out.push_back(
                    {e->info.name, databaseTypeToString(e->info.type), e->conn != nullptr});
            return out;
        }
        std::string connect(const std::string& name) override {
            return conns_.open(name);
        }
        std::shared_ptr<dearsql::IConnection> connection(const std::string& name) override {
            return conns_.connection(name);
        }
        std::pair<std::string, std::string> focus() override {
            return {focus_, ""};
        }

    private:
        CliConnections& conns_;
        std::string focus_;
    };

    // newline-delimited JSON-RPC, per the MCP stdio transport; stdout carries
    // only protocol messages, so logs go to stderr
    int serveMcp(CliConnections& conns, const std::string& focus) {
        StdioHost host(conns, focus);
        mcp::Server server(host);
        // run by hand rather than by an agent: say so on stderr (stdout is the protocol)
        if (isatty(fileno(stdin))) {
            std::cerr << "dearsql MCP server: waiting for an agent on stdin/stdout. Nothing is "
                         "printed here;\nregister it with your agent instead, e.g.\n"
                         "  claude mcp add dearsql -- dearsql --mcp\n"
                         "Ctrl-C to quit.\n";
        }
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty())
                continue;
            nlohmann::json reply;
            try {
                reply = server.handle(nlohmann::json::parse(line));
            } catch (const std::exception& e) {
                reply = {{"jsonrpc", "2.0"},
                         {"id", nullptr},
                         {"error", {{"code", -32700}, {"message", e.what()}}}};
            }
            if (!reply.is_null())
                std::cout << reply.dump() << "\n" << std::flush;
        }
        return 0;
    }

} // namespace

std::optional<int> runCli(int argc, char** argv) {
    if (argc < 2)
        return std::nullopt;
    const std::string mode = argv[1];
    if (mode == "-h" || mode == "--help") {
        std::cout << USAGE;
        return 0;
    }
    const bool mcpMode = mode == "--mcp";
    if (!mcpMode && mode != "--tui")
        return std::nullopt; // the GUI, maybe with a file to open
    std::vector<std::string> args(argv + 2, argv + argc);
#ifdef _WIN32
    // release builds are GUI-subsystem: borrow the console we were started from.
    // mcp mode keeps the agent's pipes, which are already the std handles.
    // ponytail: cmd doesn't wait for a GUI exe; `start /wait dearsql --tui` if it interleaves
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    }
#endif
    // stdout belongs to the tui screen / the mcp stream: warnings go to stderr in
    // mcp mode, nowhere in the tui
    spdlog::set_default_logger(spdlog::stderr_color_mt("dearsql"));
    spdlog::set_level(mcpMode ? spdlog::level::warn : spdlog::level::off);

    CliConnections conns;
    std::string error;
    // saved connections are always offered; a missing store is not fatal
    if (!conns.loadSaved(error) && args.empty()) {
        std::cerr << "dearsql: " << error << "\n";
    }
    std::string focus;
    for (const auto& spec : args) {
        auto name = conns.add(spec, error);
        if (name.empty()) {
            std::cerr << "dearsql: " << error << "\n";
            return 2;
        }
        if (args.size() == 1)
            focus = name;
    }

    if (mcpMode)
        return serveMcp(conns, focus);
    if (conns.entries().empty()) {
        std::cerr << USAGE;
        return 2;
    }
    return runTui(conns, focus);
}
