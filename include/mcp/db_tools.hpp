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

class S3Client;

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
        // a connection URL or database file, kept for this session only; its name,
        // or "" with error set
        virtual std::string add(const std::string& spec, std::string& error) {
            error = "only saved connections can be opened here; add it in DearSQL";
            return "";
        }
        // an S3 connection's client (type "s3"), or null; S3 tools only
        virtual std::shared_ptr<const S3Client> s3(const std::string& name) {
            return nullptr;
        }
        // library connection of an open connection, or null
        virtual std::shared_ptr<dearsql::IConnection> connection(const std::string& name) = 0;
        // what the user is looking at, used when the agent names nothing:
        // {connection, database} where database may be "db.schema"
        virtual std::pair<std::string, std::string> focus() {
            return {};
        }
        // a write on connection, shown to the user: "" to run it, else why not. the
        // cli says yes: its agent client already asks before a destructive tool
        virtual std::string approveWrite(const std::string& connection, const std::string& sql) {
            return "";
        }
        // save spec (URL or file) as a connection after the user approves; its name,
        // or "" with error set. hosts that can't leave add_connection out
        // after connect opens name: a note to append, e.g. that it was saved. the
        // one-shot `dearsql connect` saves a new url or file this way
        virtual std::string remember(const std::string& name) {
            return "";
        }
        virtual bool canSaveConnections() {
            return false;
        }
        virtual std::string saveConnection(const std::string& spec, const std::string& name,
                                           std::string& error) {
            error = "not supported";
            return "";
        }
    };

    class Server {
    public:
        // allowWrites adds execute_sql, marked destructive so the agent asks the user first
        explicit Server(Host& host, bool allowWrites = false)
            : host_(host), allowWrites_(allowWrites) {}

        // one JSON-RPC message in; the response, or null for notifications
        nlohmann::json handle(const nlohmann::json& request);

        // exposed for tests
        nlohmann::json callTool(const std::string& name, const nlohmann::json& args);

        // any thread: cancel the running tool's query server-side and refuse new
        // calls until resume(), so a host shutting down never waits out a query
        void interrupt();
        void resume();
        static const char* instructions();
        // the tool definitions (name, description, inputSchema), as tools/list sends them
        static nlohmann::json tools(bool allowWrites, bool canSaveConnections = false,
                                    bool s3 = false);
        // workflows a client can offer as slash commands (prompts/list, prompts/get)
        struct Prompt {
            std::string name;
            std::string description;
            nlohmann::json arguments;
            std::string text; // {argument} placeholders
        };
        static const std::vector<Prompt>& prompts();

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
        // saved name (fuzzy), or a URL/file the host adds; throws if neither
        std::string findConnection(const std::string& name);
        std::string connect(const nlohmann::json& args);
        std::string addConnection(const nlohmann::json& args);
        std::string searchSchema(const nlohmann::json& args);
        std::string listTables(const nlohmann::json& args);
        std::string where(const Target& target) const;
        std::string describeTable(const nlohmann::json& args);
        std::string runQuery(const nlohmann::json& args, bool write = false);
        std::string explainQuery(const nlohmann::json& args);
        std::string reviewIndexes(const nlohmann::json& args);
        std::string explainFinding(const nlohmann::json& args);
        // list_objects, read_object and, with writes, copy/delete/upload/download_object
        std::string s3Tool(const std::string& name, const nlohmann::json& args);

        // self-describing cursors; run_query pages cached by cursor, oldest evicted first
        std::string storeCursor(const std::string& key, nlohmann::json page);
        size_t cursorOffset(const std::string& id, char tag);

        Host& host_;
        bool allowWrites_;
        std::mutex mutex_; // one tool call at a time; agents rarely run them in parallel
        std::mutex activeMutex_;
        dearsql::DatabasePtr active_; // the handle the running call works on
        bool refusing_ = false;       // under activeMutex_
        std::map<std::string, Catalog> catalogs_;
        std::map<std::string, nlohmann::json> cursors_;
        std::deque<std::string> cursorOrder_;
    };

} // namespace mcp
