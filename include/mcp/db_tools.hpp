#pragma once

#include <chrono>
#include <dearsql/database.hpp>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// database tools for coding agents, served over MCP. transport-free: the GUI
// serves it over loopback http, `dearsql --mcp` over stdio. modelled on fff's
// file-search server: compact plain-text results, opaque cursors for paging,
// fuzzy "did you mean" fallbacks, forgiving arguments, and server instructions
// that tell the agent which tool to reach for.
namespace mcp {

    struct ConnectionEntry {
        std::string name;
        std::string type; // "postgresql", "sqlite", ...
        bool open = false;
    };

    // whoever owns the connections: the GUI or the cli
    class Host {
    public:
        virtual ~Host() = default;
        virtual std::vector<ConnectionEntry> connections() = 0;
        // open a known connection by exact name; "" on success, else the error
        virtual std::string connect(const std::string& name) = 0;
        // library connection of an open connection, or null
        virtual std::shared_ptr<dearsql::IConnection> connection(const std::string& name) = 0;
        // what the user is looking at, used when the agent names nothing:
        // {connection, database} where database may be "db.schema"
        virtual std::pair<std::string, std::string> focus() {
            return {};
        }
    };

    class Server {
    public:
        explicit Server(Host& host) : host_(host) {}

        // one JSON-RPC message in; the response, or null for notifications
        nlohmann::json handle(const nlohmann::json& request);

        // exposed for tests
        nlohmann::json callTool(const std::string& name, const nlohmann::json& args);
        static const char* instructions();

    private:
        struct Target {
            std::string connection;
            std::string database; // as resolved, may be "db.schema"
            dearsql::DatabasePtr db;
        };
        struct Catalog {
            std::chrono::steady_clock::time_point loaded;
            std::vector<dearsql::Table> tables; // views carry a non-empty definition or isView
            std::vector<dearsql::Table> views;
        };

        Target resolve(const nlohmann::json& args);
        const Catalog& catalog(const Target& target, bool refresh = false);
        std::vector<std::string> suggest(const Target& target, const std::string& name,
                                         size_t max = 5);

        std::string listConnections();
        std::string connect(const nlohmann::json& args);
        std::string searchSchema(const nlohmann::json& args);
        std::string listTables(const nlohmann::json& args);
        std::string describeTable(const nlohmann::json& args);
        std::string runQuery(const nlohmann::json& args);

        // opaque cursor -> remaining text pages / offset, oldest evicted first
        std::string storeCursor(nlohmann::json state);
        nlohmann::json takeCursor(const std::string& id);

        Host& host_;
        std::mutex mutex_; // one tool call at a time; agents rarely run them in parallel
        std::map<std::string, Catalog> catalogs_;
        std::map<std::string, nlohmann::json> cursors_;
        std::deque<std::string> cursorOrder_;
        uint64_t nextCursor_ = 1;
    };

} // namespace mcp
