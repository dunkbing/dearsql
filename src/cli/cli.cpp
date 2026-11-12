// the app's terminal modes: `dearsql --tui` (a terminal client over libdearsql)
// `dearsql --mcp` (a stdio MCP server with the GUI's agent database tools) and
// `dearsql --lsp` (a stdio language server for editors)
#include "cli/cli.hpp"

// before windows.h: httplib brings winsock2
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include "cli/connections.hpp"
#include "cli/lsp.hpp"
#include "cli/tui.hpp"
#include "config.hpp"
#include "mcp/db_tools.hpp"
#include "utils/cli_command.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace {

    constexpr const char* USAGE = R"(usage:
  dearsql [file]                    open the app (and the file, if given)
  dearsql --tui [connection]        browse and query in the terminal
  dearsql --mcp [connection...]     MCP server on stdin/stdout for coding agents
          [--allow-writes]          also offer execute_sql, marked destructive
  dearsql --lsp [connection]        SQL language server (completion, hover) for editors
  dearsql <command> [args]          the agent database tools as commands, no MCP needed:
          tools                     every command with its options and agent guidance
          connections, connect, search, describe, tables, query, exec (writes)
  dearsql upgrade                   install the latest release over this one
  dearsql --version                 print the version

connection: a saved DearSQL connection name, a URL (postgres://user:pass@host/db,
mysql://..., mongodb://..., redis://..., mssql://..., oracle://...), or a
.sqlite/.duckdb/.csv file. Without one, every saved connection is listed.

register with an agent, e.g.:
  claude mcp add dearsql -- dearsql --mcp
  codex mcp add dearsql -- dearsql --mcp

or let it run commands:
  dearsql search -c shop.sqlite "order user"
  dearsql query -c postgres://me@localhost/app "SELECT count(*) FROM users"
  DEARSQL_CONNECTION=prod dearsql describe orders
)";

    // the agent's view of the cli's connections; nothing is "selected", so a
    // single connection given on the command line becomes the focus
    class StdioHost : public mcp::Host {
    public:
        StdioHost(CliConnections& conns, std::string focus, bool saveOnConnect = false)
            : conns_(conns), focus_(std::move(focus)), saveOnConnect_(saveOnConnect) {}

        std::vector<mcp::ConnectionEntry> connections() override {
            reloadSaved();
            std::vector<mcp::ConnectionEntry> out;
            for (auto* e : conns_.entries())
                out.push_back(
                    {e->info.name, databaseTypeToString(e->info.type), e->conn != nullptr});
            for (const auto& info : conns_.s3Connections())
                out.push_back({info.name, "s3", true}); // stateless: nothing to open
            return out;
        }
        std::string connect(const std::string& name) override {
            return conns_.open(name);
        }
        std::string add(const std::string& spec, std::string& error) override {
            reloadSaved();
            return conns_.add(spec, error);
        }
        std::shared_ptr<dearsql::IConnection> connection(const std::string& name) override {
            return conns_.connection(name);
        }
        std::shared_ptr<const S3Client> s3(const std::string& name) override {
            return conns_.s3(name);
        }
        std::pair<std::string, std::string> focus() override {
            return {focus_, ""};
        }
        std::string remember(const std::string& name) override {
            if (!saveOnConnect_)
                return "";
            std::string error;
            auto note = conns_.save(name, error);
            return error.empty() ? note : "(" + error + ")";
        }

    private:
        // pick up connections saved after the server started (dearsql connect, the app)
        void reloadSaved() {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReload_ < std::chrono::seconds(2))
                return;
            lastReload_ = now;
            std::string error;
            conns_.loadSaved(error);
        }

        CliConnections& conns_;
        std::string focus_;
        bool saveOnConnect_;
        std::chrono::steady_clock::time_point lastReload_ = std::chrono::steady_clock::now();
    };

    // newline-delimited JSON-RPC, per the MCP stdio transport; stdout carries
    // only protocol messages, so logs go to stderr
    int serveMcp(CliConnections& conns, const std::string& focus, bool allowWrites) {
        StdioHost host(conns, focus);
        mcp::Server server(host, allowWrites);
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

    // 0.7.10 > 0.7.9
    bool isNewer(const std::string& a, const std::string& b) {
        auto parts = [](const std::string& v) {
            std::vector<int> out;
            std::istringstream in(v);
            for (std::string p; std::getline(in, p, '.');)
                out.push_back(std::atoi(p.c_str()));
            return out;
        };
        return parts(a) > parts(b);
    }

    // GET https://<host><path> into sink, following redirects (release assets go to
    // a cdn). "" on success, else the error
    std::string httpGet(const std::string& host, const std::string& path,
                        const std::function<void(const char*, size_t)>& sink) {
        httplib::Client client("https://" + host);
        client.set_follow_location(true);
        client.set_connection_timeout(15);
        client.set_read_timeout(120);
        auto res = client.Get(path, [&](const char* data, size_t n) {
            sink(data, n);
            return true;
        });
        if (!res)
            return httplib::to_string(res.error());
        return res->status == 200 ? "" : "HTTP " + std::to_string(res->status);
    }

    // `dearsql upgrade`: the latest release over this copy, through the same
    // install.sh (macOS, Linux) or msi (Windows) a fresh install uses
    int runUpgrade() {
        std::string manifest;
        auto error = httpGet("dearsql.dev", "/api/version.json",
                             [&](const char* d, size_t n) { manifest.append(d, n); });
        const auto json = nlohmann::json::parse(manifest, nullptr, false);
        const std::string latest = json.is_object() ? json.value("version", "") : "";
        if (latest.empty()) {
            std::cerr << "dearsql: could not check for updates"
                      << (error.empty() ? "" : ": " + error) << "\n";
            return 1;
        }
        if (!isNewer(latest, APP_VERSION)) {
            std::cout << "dearsql " << APP_VERSION << " is the latest version\n";
            return 0;
        }
        std::cout << "upgrading dearsql " << APP_VERSION << " to " << latest << "\n" << std::flush;
#ifdef _WIN32
        const char* temp = std::getenv("TEMP");
        const std::string msi = std::string(temp ? temp : ".") + "\\DearSQL-x64.msi";
        {
            std::ofstream out(msi, std::ios::binary);
            error = httpGet(
                "github.com",
                std::format("/dunkbing/dearsql/releases/download/v{}/DearSQL-x64.msi", latest),
                [&](const char* d, size_t n) { out.write(d, std::streamsize(n)); });
            if (error.empty() && !out)
                error = "could not write " + msi;
        }
        if (!error.empty()) {
            std::cerr << "dearsql: download failed: " << error << "\n";
            return 1;
        }
        // started detached: the installer replaces this exe, so it can't wait on us. /norestart:
        // a file in use (an open window) would otherwise let /passive restart the machine
        return std::system(std::format("start \"\" msiexec /i \"{}\" /passive /norestart", msi).c_str()) == 0
                   ? 0
                   : 1;
#else
        std::filesystem::path target = CliCommand::executable();
#ifdef __APPLE__
        target = target.parent_path().parent_path().parent_path(); // X.app/Contents/MacOS/X
        if (target.extension() != ".app") {
#else
        if (!std::getenv("APPIMAGE")) {
#endif
            std::cerr << "dearsql: this copy wasn't installed from a release; get the latest from "
                         "https://dearsql.dev\n";
            return 1;
        }
        std::string script;
        error = httpGet("dearsql.dev", "/install.sh",
                        [&](const char* d, size_t n) { script.append(d, n); });
        if (!error.empty() || script.empty()) {
            std::cerr << "dearsql: could not download the installer: " << error << "\n";
            return 1;
        }
        std::string quoted = "'";
        for (char c : target.string())
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        // install.sh still downloads with curl or wget, which every supported system has
        FILE* sh = popen(("DEARSQL_TARGET=" + quoted + "' sh").c_str(), "w");
        if (!sh) {
            std::cerr << "dearsql: could not run sh\n";
            return 1;
        }
        fwrite(script.data(), 1, script.size(), sh);
        return pclose(sh) == 0 ? 0 : 1;
#endif
    }

    // short names; the full tool names (snake or kebab case) work too
    const std::map<std::string, std::string> TOOL_ALIASES = {
        {"connections", "list_connections"}, {"tables", "list_tables"}, {"search", "search_schema"},
        {"describe", "describe_table"},      {"query", "run_query"},    {"exec", "execute_sql"}};

    std::string toolName(std::string name) {
        std::ranges::replace(name, '-', '_');
        auto it = TOOL_ALIASES.find(name);
        return it == TOOL_ALIASES.end() ? name : it->second;
    }

    bool isToolCommand(const std::string& arg) {
        const std::string name = toolName(arg);
        return name == "tools" || std::ranges::any_of(mcp::Server::tools(true, false, true),
                                                      [&](auto& t) { return t["name"] == name; });
    }

    // max-rows -> maxRows
    std::string camel(const std::string& flag) {
        std::string out;
        for (size_t i = 0; i < flag.size(); ++i)
            out += flag[i] == '-' && i + 1 < flag.size() ? (char)std::toupper(flag[++i]) : flag[i];
        return out;
    }

    void printTool(const nlohmann::json& tool) {
        const std::string name = tool["name"];
        std::string alias;
        for (const auto& [a, full] : TOOL_ALIASES)
            if (full == name)
                alias = a;
        std::string positional;
        for (const auto& r : tool["inputSchema"].value("required", nlohmann::json::array()))
            positional += " <" + r.get<std::string>() + ">";
        std::cout << "dearsql " << (alias.empty() ? name : alias) << positional
                  << (alias.empty() ? "" : "   (or " + name + ")") << "\n  "
                  << tool.value("description", "") << "\n";
        for (const auto& [prop, schema] : tool["inputSchema"]["properties"].items()) {
            std::string flag;
            for (char c : prop)
                flag += std::isupper(static_cast<unsigned char>(c))
                            ? std::string("-") + (char)std::tolower(c)
                            : std::string(1, c);
            std::cout << "    --" << flag;
            if (auto d = schema.value("description", ""); !d.empty())
                std::cout << "  " << d;
            std::cout << "\n";
        }
    }

    // `dearsql <tool> [args] [--flag value]`: the MCP tools as one-shot commands,
    // for agents that have a shell but no MCP client. same server, same text
    int runTool(CliConnections& conns, const std::string& focus, const std::string& command,
                std::vector<std::string> args) {
        StdioHost host(conns, focus, true); // `dearsql connect <url>` saves it
        mcp::Server server(host, true); // `exec` is its own command; read-only conns still refuse
        const auto tools = mcp::Server::tools(true, false, true);
        const std::string name = toolName(command);
        if (name == "tools") {
            std::cout << mcp::Server::instructions() << "\n## Commands\n"
                      << "connection (-c) and database (-d) are optional on every command: a saved "
                         "name, a URL or a file; $DEARSQL_CONNECTION sets the default. SQL may "
                         "come on stdin. Paging: pass --cursor with the same arguments.\n\n";
            for (const auto& t : tools) {
                printTool(t);
                std::cout << "\n";
            }
            return 0;
        }
        const auto tool = std::ranges::find_if(tools, [&](auto& t) { return t["name"] == name; });
        if (tool == tools.end()) {
            std::cerr << "dearsql: unknown command " << command << "\n";
            return 2;
        }
        const auto& schema = (*tool)["inputSchema"];
        auto required = schema.value("required", nlohmann::json::array());

        nlohmann::json callArgs = nlohmann::json::object();
        size_t nextPositional = 0;
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "-h" || a == "--help") {
                printTool(*tool);
                return 0;
            }
            if (a.size() > 1 && a[0] == '-' && a != "-") {
                std::string key = a.substr(a.find_first_not_of('-')), value;
                if (auto eq = key.find('='); eq != std::string::npos) {
                    value = key.substr(eq + 1);
                    key = key.substr(0, eq);
                } else if (i + 1 < args.size()) {
                    value = args[++i];
                } else {
                    std::cerr << "dearsql: " << a << " needs a value\n";
                    return 2;
                }
                key = key == "c" ? "connection" : key == "d" ? "database" : camel(key);
                callArgs[key] = value;
            } else if (nextPositional < required.size()) {
                callArgs[required[nextPositional++].get<std::string>()] = a;
            } else {
                std::cerr << "dearsql: unexpected argument '" << a << "'; see dearsql " << command
                          << " --help\n";
                return 2;
            }
        }
        // long sql is easier to pipe than to quote
        if (schema["properties"].contains("sql") &&
            (callArgs.value("sql", "-") == "-" && !isatty(fileno(stdin)))) {
            callArgs["sql"] = std::string(std::istreambuf_iterator<char>(std::cin), {});
        }
        for (const auto& r : required) {
            if (!callArgs.contains(r.get<std::string>())) {
                std::cerr << "dearsql: missing <" << r.get<std::string>() << ">\n";
                printTool(*tool);
                return 2;
            }
        }
        const auto result = server.callTool(name, callArgs);
        const std::string text = result["content"][0]["text"];
        const bool failed = result.value("isError", false);
        (failed ? std::cerr : std::cout) << text << (text.ends_with('\n') ? "" : "\n");
        return failed ? 1 : 0;
    }

} // namespace

std::optional<int> runCli(int argc, char** argv) {
    if (argc < 2)
        return std::nullopt;
    const std::string mode = argv[1];
    const bool mcpMode = mode == "--mcp";
    const bool lspMode = mode == "--lsp";
    const bool infoMode = mode == "-h" || mode == "--help" || mode == "-v" || mode == "--version";
    const bool upgradeMode = mode == "upgrade";
    const bool toolMode = !infoMode && isToolCommand(mode);
    if (!infoMode && !upgradeMode && !toolMode && !mcpMode && !lspMode && mode != "--tui")
        return std::nullopt; // the GUI, maybe with a file to open
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
    if (mode == "-v" || mode == "--version") {
        std::cout << "dearsql " << APP_VERSION << "\n";
        return 0;
    }
    if (infoMode) {
        std::cout << USAGE;
        return 0;
    }
    if (upgradeMode)
        return runUpgrade();
    std::vector<std::string> args(argv + 2, argv + argc);
    const bool allowWrites = mcpMode && std::erase(args, "--allow-writes") > 0;
    // stdout belongs to the tui screen / the protocol stream: warnings go to stderr
    // in mcp and lsp mode, nowhere in the tui
    spdlog::set_default_logger(spdlog::stderr_color_mt("dearsql"));
    spdlog::set_level(mcpMode || lspMode || toolMode ? spdlog::level::warn : spdlog::level::off);
    if (lspMode && args.size() > 1) {
        std::cerr << "dearsql: --lsp takes one connection\n";
        return 2;
    }

    CliConnections conns;
    std::string error;
    // saved connections are always offered; a missing store is not fatal
    if (!conns.loadSaved(error) && (args.empty() || toolMode)) {
        std::cerr << "dearsql: " << error << "\n";
    }
    std::string focus;
    // agents set the connection once instead of on every call
    if (const char* env = std::getenv("DEARSQL_CONNECTION"); env && *env && (toolMode || mcpMode)) {
        focus = conns.add(env, error);
        if (focus.empty()) {
            std::cerr << "dearsql: DEARSQL_CONNECTION: " << error << "\n";
            return 2;
        }
    }
    if (toolMode)
        return runTool(conns, focus, mode, std::move(args));
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
        return serveMcp(conns, focus, allowWrites);
    if (lspMode)
        return runLsp(conns, focus);
    if (conns.entries().empty()) {
        std::cerr << USAGE;
        return 2;
    }
    return runTui(conns, focus);
}
