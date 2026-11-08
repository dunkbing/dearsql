#include "mcp/db_tools.hpp"

#include "database/s3_client.hpp"

#include "mcp/fuzzy.hpp"
#include "utils/sql_guard.hpp"
#include <algorithm>
#include <cstring>
#include <dearsql/completion.hpp>
#include <dearsql/explain.hpp>
#include <dearsql/findings.hpp>
#include <format>
#include <numeric>
#include <regex>
#include <set>

using json = nlohmann::json;

namespace mcp {

    namespace {

        constexpr auto CATALOG_TTL = std::chrono::seconds(120);
        constexpr size_t MAX_CURSORS = 20;
        constexpr size_t MAX_OUTPUT = 30 * 1024; // keep a page well inside an agent's context
        constexpr size_t MAX_CELL = 200;
        constexpr int FETCH_ROWS = 1000;

        struct ToolError : std::runtime_error {
            using std::runtime_error::runtime_error;
        };

        // first present key among aliases; agents mix up names, so accept them all
        std::string str(const json& args, std::initializer_list<const char*> keys) {
            for (const char* k : keys) {
                if (auto it = args.find(k); it != args.end()) {
                    if (it->is_string())
                        return it->get<std::string>();
                    if (it->is_number())
                        return it->dump();
                }
            }
            return {};
        }

        // numbers arrive as 20, 20.0 or "20"
        int num(const json& args, std::initializer_list<const char*> keys, int def, int lo,
                int hi) {
            for (const char* k : keys) {
                auto it = args.find(k);
                if (it == args.end())
                    continue;
                double v = 0;
                if (it->is_number())
                    v = it->get<double>();
                else if (it->is_string())
                    v = std::atof(it->get<std::string>().c_str());
                else
                    continue;
                if (v > 0)
                    return std::clamp(static_cast<int>(v + 0.5), lo, hi);
            }
            return def;
        }

        bool hasSchemas(dearsql::DatabaseType t) {
            using enum dearsql::DatabaseType;
            return dearsql::isPostgresFamily(t) || t == MSSQL;
        }

        std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string qualified(const dearsql::Table& t) {
            return t.schema.empty() ? t.name : t.schema + "." + t.name;
        }

        // not text: control bytes or invalid UTF-8 (BLOBs come back as raw bytes)
        bool binary(std::string_view v) {
            for (size_t i = 0; i < v.size();) {
                const auto c = static_cast<unsigned char>(v[i]);
                if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
                    return true;
                const size_t len = c < 0x80         ? 1
                                   : (c >> 5) == 6  ? 2
                                   : (c >> 4) == 14 ? 3
                                   : (c >> 3) == 30 ? 4
                                                    : 0;
                if (len == 0 || i + len > v.size())
                    return true;
                for (size_t k = 1; k < len; ++k)
                    if ((static_cast<unsigned char>(v[i + k]) >> 6) != 2)
                        return true;
                i += len;
            }
            return false;
        }

        std::string cell(const std::string& v) {
            if (dearsql::isNullSentinel(v))
                return "NULL";
            if (dearsql::isBoolSentinel(v))
                return dearsql::boolSentinelValue(v) ? "true" : "false";
            if (binary(v)) {
                if (v.size() > 32)
                    return std::format("<blob {} bytes>", v.size());
                std::string hex = "0x";
                for (unsigned char c : v)
                    hex += std::format("{:02X}", c);
                return hex;
            }
            std::string out;
            out.reserve(std::min(v.size(), MAX_CELL + 16));
            for (char c : v) {
                if (out.size() >= MAX_CELL) {
                    out += std::format("…(+{} chars)", v.size() - MAX_CELL);
                    break;
                }
                out += c == '\n'   ? std::string("\\n")
                       : c == '\t' ? std::string(" ")
                       : c == '|'  ? std::string("\\|")
                                   : std::string(1, c);
            }
            return out;
        }

        // read-only allowlists for the non-SQL backends
        bool redisReadOnly(const std::string& command) {
            static const std::set<std::string> allowed = {
                "get",     "mget",          "strlen",    "getrange", "exists",    "type",
                "ttl",     "pttl",          "keys",      "scan",     "hget",      "hmget",
                "hgetall", "hkeys",         "hvals",     "hlen",     "hscan",     "lrange",
                "llen",    "lindex",        "smembers",  "scard",    "sismember", "sscan",
                "zrange",  "zrangebyscore", "zrevrange", "zcard",    "zscore",    "zscan",
                "xrange",  "xrevrange",     "xlen",      "info",     "dbsize",    "memory",
                "object",  "ping",          "time"};
            std::string first = command.substr(0, command.find_first_of(" \t\n"));
            return allowed.contains(lower(first));
        }

        bool mongoReadOnly(const std::string& text) {
            try {
                auto doc = json::parse(text);
                std::string cmd = doc.value("command", "");
                if (cmd.empty() && doc.is_object() && !doc.empty())
                    cmd = doc.begin().key(); // raw runCommand form: first key is the command
                static const std::set<std::string> allowed = {
                    "find",     "aggregate",       "count",       "countDocuments",
                    "distinct", "listCollections", "listIndexes", "estimatedDocumentCount",
                    "dbStats",  "collStats"};
                if (!allowed.contains(cmd))
                    return false;
                // $out / $merge write from a pipeline
                const std::string dumped = doc.dump();
                return dumped.find("\"$out\"") == std::string::npos &&
                       dumped.find("\"$merge\"") == std::string::npos;
            } catch (...) {
                return false;
            }
        }

        bool readOnly(dearsql::DatabaseType type, const std::string& text) {
            if (type == dearsql::DatabaseType::REDIS)
                return redisReadOnly(text);
            if (type == dearsql::DatabaseType::MONGODB)
                return mongoReadOnly(text);
            return SqlGuard::isReadOnly(text);
        }

        // the unknown name in a "no such table/column" error, per dialect
        std::pair<std::string, bool> missingName(const std::string& error) {
            static const std::vector<std::pair<std::regex, bool>> patterns = {
                {std::regex(R"(relation \"([^\"]+)\" does not exist)"), true},
                {std::regex(R"(Table '([^']+)' doesn't exist)"), true},
                {std::regex(R"(no such table: (\S+))"), true},
                {std::regex(R"(Invalid object name '([^']+)')"), true},
                {std::regex(R"(Table with name (\S+) does not exist)"), true},
                {std::regex(R"(unconfigured table (\S+))"), true},
                {std::regex(R"(column \"([^\"]+)\" does not exist)"), false},
                {std::regex(R"(Unknown column '([^']+)')"), false},
                {std::regex(R"(no such column: (\S+))"), false},
                {std::regex(R"(Invalid column name '([^']+)')"), false},
                {std::regex(R"(Referenced column \"([^\"]+)\" not found)"), false},
                {std::regex(R"(ORA-00904: \"?([^\":]+)\"?: invalid identifier)"), false},
            };
            std::smatch m;
            for (const auto& [re, isTable] : patterns) {
                if (std::regex_search(error, m, re)) {
                    std::string name = m[1];
                    // mysql reports db.table, postgres may report schema.table
                    if (auto dot = name.rfind('.'); dot != std::string::npos)
                        name = name.substr(dot + 1);
                    return {name, isTable};
                }
            }
            return {"", false};
        }

        json toolDef(const char* name, const char* description, json properties,
                     std::vector<std::string> required = {}) {
            json schema = {{"type", "object"}, {"properties", std::move(properties)}};
            if (!required.empty())
                schema["required"] = required;
            return {
                {"name", name},
                {"description", description},
                {"inputSchema", std::move(schema)},
                {"annotations",
                 {{"readOnlyHint", true}, {"destructiveHint", false}, {"openWorldHint", false}}}};
        }

        json textResult(const std::string& text, bool isError = false) {
            return {{"content", json::array({{{"type", "text"}, {"text", text}}})},
                    {"isError", isError}};
        }

        const json TARGET_PROPS = {
            {"connection",
             {{"type", "string"},
              {"description",
               "Saved connection name (outside the app also a URL or database "
               "file). Default: the one selected in DearSQL, or the only open one."}}},
            {"database",
             {{"type", "string"},
              {"description", "Database (or \"db.schema\" for Postgres/SQL Server). Default: the "
                              "selected or default one."}}},
        };

        json withTarget(json props) {
            for (auto& [k, v] : TARGET_PROPS.items())
                props[k] = v;
            return props;
        }

    } // namespace

    const char* Server::instructions() {
        return "DearSQL gives you the user's database connections. Results are compact text; SQL "
               "NULL prints as NULL.\n\n"
               "## Which tool\n"
               "- search_schema: DEFAULT first step. Fuzzy-finds tables, views and columns by "
               "name (\"user\", \"created at\", typos are fine). Use it instead of guessing names "
               "or listing everything.\n"
               "- describe_table: columns, types, keys, indexes and foreign keys of one table. "
               "Call it before writing a query that joins or filters on that table.\n"
               "- explain_query: the plan of a statement with warnings (full scans, repeated "
               "scans, "
               "big sorts). Use it when a query is slow or before suggesting an index.\n"
               "- review_indexes: foreign keys with no index and indexes another covers, from "
               "the schema alone. explain_finding <code> explains any [code] in a result.\n"
               "- run_query: read-only SQL (SELECT/WITH/SHOW/EXPLAIN, read-only PRAGMA on "
               "SQLite), or a read-only command on "
               "Redis/MongoDB. Returns up to maxRows rows and a cursor for more.\n"
               "- list_tables: the whole map (tables and views with column counts and sizes), "
               "paged. Use when search_schema is not enough.\n"
               "- list_connections: the user's saved connections. Naming one in any tool opens "
               "it; connect opens one explicitly.\n"
               "- list_objects / read_object: S3 object storage connections (type s3): "
               "buckets, folders and files, and the start of a file as text.\n\n"
               "## Rules\n"
               "1. Never guess table or column names: search_schema, then describe_table.\n"
               "2. Put LIMIT (TOP on SQL Server) on exploratory queries; aggregate in SQL "
               "instead of paging through rows.\n"
               "3. Writes (INSERT/UPDATE/DELETE/DDL) are rejected. Show the user the SQL instead.\n"
               "4. connection and database are optional and default to what the user has "
               "selected in DearSQL.\n"
               "5. When a result ends with \"cursor: N\", pass that cursor to the same tool for "
               "the next page instead of re-running with OFFSET.\n";
    }

    json Server::tools(bool allowWrites, bool canSaveConnections, bool s3) {
        json maxResults = {{"type", "integer"}, {"description", "Max results (default 20)."}};
        json cursor = {{"type", "string"},
                       {"description", "Cursor from a previous result, for the next page."}};
        json tools = json::array(
            {toolDef("search_schema",
                     "Fuzzy search over table, view and column NAMES of a database. "
                     "Words narrow the search (\"orders user\" finds orders.user_id). "
                     "Typo-tolerant, case and snake/camel insensitive. Start here.",
                     withTarget({{"query",
                                  {{"type", "string"},
                                   {"description", "1-3 words, e.g. \"invoice total\""}}},
                                 {"maxResults", maxResults},
                                 {"cursor", cursor}}),
                     {"query"}),
             toolDef("describe_table",
                     "Columns (type, null, default, key), indexes and foreign keys in "
                     "and out of one table or view. Name may be schema-qualified; close "
                     "misspellings are resolved.",
                     withTarget({{"table", {{"type", "string"}}}}), {"table"}),
             toolDef("run_query",
                     "Run one read-only statement and return rows as compact text. Use "
                     "LIMIT. Read-only PRAGMA works on SQLite; Redis/MongoDB take their read "
                     "commands instead of SQL.",
                     withTarget({{"sql", {{"type", "string"}}},
                                 {"maxRows",
                                  {{"type", "integer"},
                                   {"description",
                                    "Rows per page (default 50, max 500). A page also stops "
                                    "at 30 KB of text, and at most 1000 rows are fetched."}}},
                                 {"cursor", cursor}}),
                     {"sql"}),
             toolDef("explain_query",
                     "The query plan of one statement as an indented tree (rows, cost, scan and "
                     "join conditions), plus warnings from fixed rules: full scans a filter mostly "
                     "throws away, full scans repeated per outer row, big or spilled sorts, "
                     "row estimates off by 10x. Plans only, nothing runs unless analyze is set. "
                     "PostgreSQL, MySQL 8 and SQLite. Use it before suggesting an index.",
                     withTarget({{"sql", {{"type", "string"}}},
                                 {"analyze",
                                  {{"type", "boolean"},
                                   {"description",
                                    "Run the statement for actual rows and times (EXPLAIN "
                                    "ANALYZE). Read-only statements only; it takes as long as "
                                    "the query."}}}}),
                     {"sql"}),
             toolDef(
                 "review_indexes",
                 "Index checks from the schema alone: foreign keys with no index, and indexes "
                 "another index already covers. Each finding has a code (explain_finding) and "
                 "a statement to consider. Any SQL database.",
                 withTarget({{"table",
                              {{"type", "string"},
                               {"description", "One table; default every table (up to 200)."}}}})),
             toolDef("explain_finding",
                     "What a finding code from explain_query or review_indexes means, how to "
                     "confirm it, the usual fix and when to leave it. Quote it to the user rather "
                     "than improvising.",
                     {{"code", {{"type", "string"}, {"description", "e.g. unindexed-fk"}}}},
                     {"code"}),
             toolDef("list_tables",
                     "Every table and view with column count and size, paged. Prefer "
                     "search_schema when you know roughly what you want.",
                     withTarget({{"maxResults", maxResults}, {"cursor", cursor}})),
             toolDef("list_connections",
                     "The user's saved connections, which are open, and the databases "
                     "on open ones.",
                     json::object()),
             toolDef("connect",
                     "Open one of the user's saved connections by name, or a connection URL or "
                     "database file path (the `dearsql connect` command also saves it).",
                     {{"name", {{"type", "string"}}}}, {"name"})});
        const json s3conn = {{"type", "string"},
                             {"description", "S3 connection name. Default: the selected or only "
                                             "S3 connection."}};
        const json bucket = {{"type", "string"}};
        const json key = {{"type", "string"}, {"description", "Object key; a folder ends in '/'."}};
        if (s3) { // only for hosts that have S3 connections
            tools.push_back(
                toolDef("list_objects",
                        "S3 object storage: buckets (no bucket given), or the folders and "
                        "files under a prefix of one bucket, one level, paged.",
                        {{"connection", s3conn},
                         {"bucket", bucket},
                         {"prefix", {{"type", "string"}, {"description", "e.g. \"logs/2026/\""}}},
                         {"cursor", cursor}}));
            tools.push_back(
                toolDef("read_object",
                        "S3 object storage: the start of a file as text (CSV, JSON, logs...).",
                        {{"connection", s3conn},
                         {"bucket", bucket},
                         {"key", key},
                         {"maxBytes",
                          {{"type", "integer"}, {"description", "Default 16384, max 1048576."}}}},
                        {"bucket", "key"}));
            if (allowWrites) {
                auto write = [&](const char* name, const char* description, json props,
                                 std::vector<std::string> required, bool destructive) {
                    auto def = toolDef(name, description, std::move(props), std::move(required));
                    def["annotations"]["readOnlyHint"] = false;
                    def["annotations"]["destructiveHint"] = destructive;
                    tools.push_back(std::move(def));
                };
                write(
                    "copy_object",
                    "S3 object storage: copy (or move) a file, or a folder with everything in it, "
                    "within one connection. Overwrites the target.",
                    {{"connection", s3conn},
                     {"bucket", bucket},
                     {"key", key},
                     {"toBucket",
                      {{"type", "string"}, {"description", "Default: the same bucket."}}},
                     {"toKey", key},
                     {"move", {{"type", "boolean"}, {"description", "Delete the source after."}}}},
                    {"bucket", "key", "toKey"}, true);
                write("delete_object",
                      "S3 object storage: delete a file, or a folder (key ending '/') with "
                      "everything in it.",
                      {{"connection", s3conn}, {"bucket", bucket}, {"key", key}}, {"bucket", "key"},
                      true);
                write("upload_object", "S3 object storage: upload a local file.",
                      {{"connection", s3conn},
                       {"path", {{"type", "string"}, {"description", "Local file."}}},
                       {"bucket", bucket},
                       {"key", key}},
                      {"path", "bucket", "key"}, true);
                write("download_object", "S3 object storage: save a file to a local path.",
                      {{"connection", s3conn},
                       {"bucket", bucket},
                       {"key", key},
                       {"path", {{"type", "string"}, {"description", "Local file to write."}}}},
                      {"bucket", "key", "path"}, true); // may overwrite a local file
            }
        }
        if (allowWrites) {
            auto def =
                toolDef("execute_sql",
                        "Run one statement that changes data or schema (INSERT, UPDATE, DELETE, "
                        "DDL). Only when the user asked for the change; use run_query to read.",
                        withTarget({{"sql", {{"type", "string"}}}}), {"sql"});
            def["annotations"]["readOnlyHint"] = false;
            def["annotations"]["destructiveHint"] = true;
            tools.push_back(std::move(def));
        }
        if (canSaveConnections) {
            auto def =
                toolDef("add_connection",
                        "Save a new connection in DearSQL (the user approves it first) and open "
                        "it. Only for a database the user wants added, e.g. a local one you just "
                        "started.",
                        {{"url",
                          {{"type", "string"},
                           {"description", "Connection URL (postgres://user:pass@host:5432/db, "
                                           "mysql://, ...) or a SQLite/DuckDB/CSV file path"}}},
                         {"name", {{"type", "string"}, {"description", "Optional display name"}}}},
                        {"url"});
            def["annotations"]["readOnlyHint"] = false;
            tools.push_back(std::move(def));
        }
        return tools;
    }

    const std::vector<Server::Prompt>& Server::prompts() {
        static const std::vector<Prompt> all = {
            {"diagnose-slow-query", "Find out why a query is slow and what would fix it",
             json::array(
                 {{{"name", "sql"}, {"description", "The slow statement"}, {"required", true}},
                  {{"name", "connection"},
                   {"description", "Saved connection; default: the selected one"}}}),
             "Find out why this query is slow on connection {connection}, without changing "
             "anything:\n\n{sql}\n\n"
             "1. explain_query it. If it is read-only and the user is fine waiting for it, "
             "explain_query again with analyze for real row counts and times.\n"
             "2. For each [code] in the warnings, explain_finding the code and do what its "
             "\"How to confirm\" says (describe_table, count rows with run_query).\n"
             "3. review_indexes on the tables involved.\n"
             "4. Report: the step that costs the most, the evidence, and the smallest fix "
             "(usually one CREATE INDEX or a rewrite), with the exact statement. Say what you "
             "could not confirm. Do not run the fix."},
            {"review-schema", "Check a database's indexes and foreign keys",
             json::array({{{"name", "connection"},
                           {"description", "Saved connection; default: the selected one"}},
                          {{"name", "database"}, {"description", "Database or db.schema"}}}),
             "Review the indexes of database {database} on connection {connection}, without "
             "changing anything.\n\n"
             "1. review_indexes.\n"
             "2. explain_finding each code that comes back, and check the findings worth "
             "acting on (table sizes from list_tables, how the columns are used).\n"
             "3. Report a short prioritized list: what to add or drop, why, the statement, and "
             "what to check first. Group the rest as low priority. Do not run anything that "
             "changes the schema."},
        };
        return all;
    }

    json Server::handle(const json& req) {
        if (!req.contains("id"))
            return nullptr; // notification
        const std::string method = req.value("method", "");
        json res = {{"jsonrpc", "2.0"}, {"id", req["id"]}};
        const json params = req.value("params", json::object());

        if (method == "initialize") {
            res["result"] = {
                {"protocolVersion", params.value("protocolVersion", "2025-03-26")},
                {"capabilities", {{"tools", json::object()}, {"prompts", json::object()}}},
                {"serverInfo", {{"name", "dearsql"}, {"version", "1.0"}}},
                {"instructions", std::string(instructions()) +
                                     (allowWrites_ ? "\nWrites are enabled: execute_sql runs "
                                                     "a change the user asked for. Rule 3 "
                                                     "applies to run_query only.\n"
                                                   : "")}};
        } else if (method == "ping") {
            res["result"] = json::object();
        } else if (method == "tools/list") {
            res["result"] = {
                {"tools", tools(allowWrites_, host_.canSaveConnections(),
                                std::ranges::any_of(host_.connections(), [](const auto& c) {
                                    return c.type == "s3";
                                }))}};
        } else if (method == "prompts/list") {
            json list = json::array();
            for (const auto& p : prompts())
                list.push_back(
                    {{"name", p.name}, {"description", p.description}, {"arguments", p.arguments}});
            res["result"] = {{"prompts", list}};
        } else if (method == "prompts/get") {
            const std::string name = params.value("name", "");
            const json args = params.value("arguments", json::object());
            const auto& all = prompts();
            const auto it = std::ranges::find(all, name, &Prompt::name);
            if (it == all.end()) {
                res["error"] = {{"code", -32602}, {"message", "Unknown prompt: " + name}};
            } else {
                // {name} in the text becomes the argument
                std::string text = it->text;
                for (const auto& a : it->arguments) {
                    const std::string key = a["name"];
                    const std::string value = str(args, {key.c_str()});
                    const std::string with = value.empty() ? "(not given)" : value;
                    for (size_t at = 0; (at = text.find("{" + key + "}", at)) != std::string::npos;
                         at += with.size())
                        text.replace(at, key.size() + 2, with);
                }
                res["result"] = {
                    {"description", it->description},
                    {"messages", json::array({{{"role", "user"},
                                               {"content", {{"type", "text"}, {"text", text}}}}})}};
            }
        } else if (method == "tools/call") {
            res["result"] =
                callTool(params.value("name", ""), params.value("arguments", json::object()));
        } else {
            res["error"] = {{"code", -32601}, {"message", "Method not found: " + method}};
        }
        return res;
    }

    void Server::interrupt() {
        dearsql::DatabasePtr db;
        {
            std::lock_guard lock(activeMutex_);
            refusing_ = true;
            db = active_;
        }
        if (db)
            db->cancel();
    }

    void Server::resume() {
        std::lock_guard lock(activeMutex_);
        refusing_ = false;
    }

    json Server::callTool(const std::string& name, const json& rawArgs) {
        const json args = rawArgs.is_object() ? rawArgs : json::object();
        std::lock_guard lock(mutex_);
        {
            std::lock_guard active(activeMutex_);
            if (refusing_)
                return textResult("DearSQL closed the database tools.", true);
        }
        // the handle resolve() picked stays cancellable until the call returns
        struct ClearActive {
            Server& s;
            ~ClearActive() {
                std::lock_guard active(s.activeMutex_);
                s.active_.reset();
            }
        } clearActive{*this};
        try {
            if (name == "list_connections")
                return textResult(listConnections());
            if (name == "connect")
                return textResult(connect(args));
            if (name == "search_schema")
                return textResult(searchSchema(args));
            if (name == "list_tables")
                return textResult(listTables(args));
            if (name == "describe_table")
                return textResult(describeTable(args));
            if (name == "add_connection" && host_.canSaveConnections())
                return textResult(addConnection(args));
            if (name == "execute_sql" && allowWrites_)
                return textResult(runQuery(args, true));
            if (name == "run_query")
                return textResult(runQuery(args));
            if (name == "explain_query")
                return textResult(explainQuery(args));
            if (name == "review_indexes")
                return textResult(reviewIndexes(args));
            if (name == "explain_finding")
                return textResult(explainFinding(args));
            if (name == "list_objects" || name == "read_object" ||
                (allowWrites_ && (name == "copy_object" || name == "delete_object" ||
                                  name == "upload_object" || name == "download_object")))
                return textResult(s3Tool(name, args));
            return textResult("Unknown tool: " + name, true);
        } catch (const ToolError& e) {
            return textResult(e.what(), true);
        } catch (const std::exception& e) {
            return textResult(std::string("Failed: ") + e.what(), true);
        }
    }

    // ---------- targeting ----------

    Server::Target Server::resolve(const json& args) {
        const auto conns = host_.connections();
        const auto [focusConn, focusDb] = host_.focus();

        std::string conn = str(args, {"connection", "conn", "connection_name"});
        if (conn.empty())
            conn = focusConn;
        if (conn.empty()) {
            std::vector<std::string> open;
            for (const auto& c : conns) {
                if (c.open && c.type != "s3")
                    open.push_back(c.name);
            }
            if (open.size() == 1) {
                conn = open[0];
            } else if (open.empty()) {
                throw ToolError(
                    "No connection is open. Call list_connections, then connect to one.");
            } else {
                std::string names;
                for (const auto& n : open)
                    names += (names.empty() ? "" : ", ") + n;
                throw ToolError("Several connections are open (" + names + "); pass connection.");
            }
        }

        const std::string name = findConnection(conn);
        // a connection the agent named (or the user selected) opens on first use
        auto lib = host_.connection(name);
        if (!lib) {
            if (auto err = host_.connect(name); !err.empty())
                throw ToolError("Could not open " + name + ": " + err);
            lib = host_.connection(name);
        }
        if (!lib)
            throw ToolError("'" + name + "' is not open. Call connect.");

        std::string db = str(args, {"database", "db"});
        std::string schema = str(args, {"schema"});
        if (db.empty() && name == focusConn)
            db = focusDb;
        if (db.empty())
            db = lib->info().database;
        if (hasSchemas(lib->type()) && schema.empty()) {
            if (auto dot = db.find('.'); dot != std::string::npos) {
                schema = db.substr(dot + 1);
                db = db.substr(0, dot);
            }
        }

        auto handle = lib->database(db);
        if (!handle)
            throw ToolError("Could not open database '" + db + "' on " + name + ".");
        {
            std::lock_guard active(activeMutex_);
            active_ = handle;
            if (refusing_)
                throw ToolError("DearSQL closed the database tools.");
        }
        if (!schema.empty()) {
            auto s = handle->schema(schema);
            if (!s)
                throw ToolError("No schema '" + schema + "' in " + db + ".");
            handle = s;
        }
        std::string label = db;
        if (!schema.empty())
            label += "." + schema;
        return {name, label, std::move(handle)};
    }

    const Server::Catalog& Server::catalog(const Target& target, bool refresh) {
        const std::string key = target.connection + "\n" + target.database;
        auto it = catalogs_.find(key);
        const auto now = std::chrono::steady_clock::now();
        if (it != catalogs_.end() && !refresh && now - it->second.loaded < CATALOG_TTL)
            return it->second;
        Catalog c;
        c.loaded = now;
        c.tables = target.db->tables();
        c.views = target.db->views();
        auto mv = target.db->materializedViews();
        c.views.insert(c.views.end(), mv.begin(), mv.end());
        return catalogs_[key] = std::move(c);
    }

    std::vector<std::string> Server::suggest(const Target& target, const std::string& name,
                                             size_t max) {
        std::vector<std::pair<int, std::string>> scored;
        const auto& cat = catalog(target);
        for (const auto* list : {&cat.tables, &cat.views}) {
            for (const auto& t : *list) {
                if (int s = std::max(fuzzy::score(name, t.name), fuzzy::score(name, qualified(t))))
                    scored.emplace_back(s, qualified(t));
            }
        }
        std::ranges::sort(scored, std::greater<>());
        std::vector<std::string> out;
        for (const auto& [s, n] : scored) {
            if (out.size() >= max)
                break;
            out.push_back(n);
        }
        return out;
    }

    // ---------- cursors ----------
    // a cursor carries its own position, so it outlives this server (a `dearsql
    // run_query` call is one process): "t40" list_tables, "s40" search_schema,
    // "q40-1a2b3c4d" run_query (offset, hash of connection, database and sql). run_query also keeps
    // its fetched rows here so a cached page needs no re-run

    namespace {
        uint32_t sqlHash(const std::string& sql) {
            uint32_t h = 2166136261u; // fnv-1a
            for (unsigned char c : sql)
                h = (h ^ c) * 16777619u;
            return h;
        }
        const char* cursorTool(char tag) {
            return tag == 't' ? "list_tables" : tag == 's' ? "search_schema" : "run_query";
        }
    } // namespace

    std::string Server::storeCursor(const std::string& key, json page) {
        const std::string id = std::format("q{}-{:08x}", page.value("offset", 0), sqlHash(key));
        cursors_[id] = std::move(page);
        cursorOrder_.push_back(id);
        while (cursorOrder_.size() > MAX_CURSORS) {
            cursors_.erase(cursorOrder_.front());
            cursorOrder_.pop_front();
        }
        return id;
    }

    size_t Server::cursorOffset(const std::string& id, char tag) {
        const char* tool = cursorTool(tag);
        if (id.empty() || !std::strchr("tsq", id[0]) || id.size() < 2 ||
            !std::isdigit(static_cast<unsigned char>(id[1])))
            throw ToolError("Cursor '" + id + "' is not one of " + tool +
                            "'s; call it without the cursor.");
        if (id[0] != tag)
            throw ToolError("Cursor '" + id + "' belongs to " + cursorTool(id[0]) + ", not " +
                            tool + "; call " + tool + " without it.");
        return std::strtoull(id.c_str() + 1, nullptr, 10);
    }

    // "app_state.db (local)" for messages; a bare connection name when it has one database
    std::string Server::where(const Target& target) const {
        return target.database.empty() ? target.connection
                                       : target.database + " (" + target.connection + ")";
    }

    // ---------- tools ----------

    std::string Server::listConnections() {
        const auto conns = host_.connections();
        if (conns.empty())
            return "DearSQL has no saved connections.";
        const auto focus = host_.focus().first;
        std::string out;
        for (const auto& c : conns) {
            out += std::format("{} {}  {}  {}", c.name == focus ? "*" : "-", c.name, c.type,
                               c.open ? "open" : "closed");
            if (auto lib = c.open ? host_.connection(c.name) : nullptr) {
                try {
                    auto dbs = lib->databases();
                    std::string names;
                    for (size_t i = 0; i < dbs.size() && i < 15; ++i)
                        names += (i ? ", " : "") + dbs[i]->name();
                    if (dbs.size() > 15)
                        names += std::format(" (+{} more)", dbs.size() - 15);
                    if (!names.empty() && c.type != "sqlite" && c.type != "duckdb")
                        out += "  databases: " + names;
                } catch (const std::exception& e) {
                    out += std::string("  (") + e.what() + ")";
                }
            }
            out += "\n";
        }
        out += std::ranges::any_of(conns, [&](const auto& c) { return c.name == focus; })
                   ? "* = selected in DearSQL. Closed ones open with connect."
                   : "None selected in DearSQL: pass connection unless only one is open. "
                     "Closed ones "
                     "open with connect.";
        return out;
    }

    std::string Server::connect(const json& args) {
        const std::string name = str(args, {"name", "connection"});
        if (name.empty())
            throw ToolError("Missing 'name'.");
        const std::string found = findConnection(name);
        if (host_.connection(found))
            return found + " is already open.";
        if (auto err = host_.connect(found); !err.empty())
            throw ToolError("Could not connect to " + found + ": " + err);
        const std::string note = host_.remember(found);
        return found + ": connection OK." + (note.empty() ? "" : " " + note);
    }

    std::string Server::addConnection(const json& args) {
        const std::string spec = str(args, {"url", "uri", "path", "connection"});
        if (spec.empty())
            throw ToolError("Missing 'url'.");
        std::string error;
        const std::string name = host_.saveConnection(spec, str(args, {"name"}), error);
        if (name.empty())
            throw ToolError("Not saved: " + error);
        if (auto err = host_.connect(name); !err.empty())
            throw ToolError("Saved as " + name + ", but it did not open: " + err);
        return "Saved and connected to " + name + ".";
    }

    std::string Server::findConnection(const std::string& name) {
        // exact (case-insensitive) or the best close match
        const ConnectionEntry* entry = nullptr;
        int best = 0;
        const auto conns = host_.connections();
        for (const auto& c : conns) {
            const int s = lower(c.name) == lower(name) ? 2000 : fuzzy::score(name, c.name);
            if (s > best) {
                best = s;
                entry = &c;
            }
        }
        if (entry && best >= 400)
            return entry->name;
        std::string error;
        if (auto added = host_.add(name, error); !added.empty())
            return added;
        throw ToolError("No connection named '" + name + "' (" + error +
                        "). Call list_connections.");
    }

    std::string Server::searchSchema(const json& args) {
        const std::string query = str(args, {"query", "pattern", "q", "name"});
        if (query.empty())
            throw ToolError("Missing 'query'.");
        const int maxResults = num(args, {"maxResults", "max_results", "limit"}, 20, 1, 200);
        int offset = 0;
        if (auto c = str(args, {"cursor"}); !c.empty())
            offset = cursorOffset(c, 's');

        auto target = resolve(args);
        const auto& cat = catalog(target);

        std::vector<std::string> words;
        for (size_t i = 0; i < query.size();) {
            size_t j = query.find_first_of(" \t,", i);
            if (j == std::string::npos)
                j = query.size();
            if (j > i)
                words.push_back(query.substr(i, j - i));
            i = j + 1;
        }

        struct Hit {
            int score;
            std::string kind;
            std::string name;
            std::string detail;
        };
        std::vector<Hit> hits;
        // each word hits the table name or the column name; more words hit ranks
        // first, so one misspelled word lowers a hit instead of dropping it
        auto scoreAll = [&](const std::string& tableName, const std::string& colName) {
            int total = 0, matched = 0;
            for (const auto& w : words) {
                int s = fuzzy::score(w, tableName);
                if (!colName.empty())
                    s = std::max(s, fuzzy::score(w, colName));
                if (s) {
                    total += s;
                    ++matched;
                }
            }
            return matched ? matched * 10000 + total : 0;
        };
        for (const auto* list : {&cat.tables, &cat.views}) {
            const bool views = list == &cat.views;
            for (const auto& t : *list) {
                const int tableScore = std::max(scoreAll(t.name, ""), scoreAll(qualified(t), ""));
                if (tableScore) {
                    hits.push_back({tableScore + 1, views ? "view" : "table", qualified(t),
                                    std::format("{} cols", t.columns.size())});
                }
                for (const auto& col : t.columns) {
                    if (int s = scoreAll(t.name, col.name); s) {
                        // a column must match by its own name, not only via its table
                        if (std::ranges::none_of(words, [&](const std::string& w) {
                                return fuzzy::score(w, col.name) > 0;
                            }))
                            continue;
                        // a table that matches the whole query outranks its columns
                        s = s / 10000 * 10000 + s % 10000 * 8 / 10;
                        if (tableScore)
                            s = std::min(s, tableScore);
                        hits.push_back({s, "column", qualified(t) + "." + col.name, col.type});
                    }
                }
            }
        }
        std::ranges::stable_sort(hits,
                                 [](const Hit& a, const Hit& b) { return a.score > b.score; });

        const size_t total = cat.tables.size() + cat.views.size();
        if (hits.empty())
            return std::format("0 matches for '{}' in {} tables/views of {}. Try one shorter "
                               "word, or list_tables.",
                               query, total, where(target));

        std::string out;
        if (offset == 0 && hits[0].kind != "column" &&
            (hits.size() == 1 || hits[0].score / 10000 > hits[1].score / 10000 ||
             hits[0].score % 10000 >= 1000 || hits[0].score % 10000 > hits[1].score % 10000 * 2))
            out += std::format("→ describe_table {} (best match)\n", hits[0].name);
        const size_t end = std::min(hits.size(), static_cast<size_t>(offset + maxResults));
        if (hits.size() > static_cast<size_t>(maxResults) || offset > 0)
            out += std::format("{}-{} of {} matches\n", offset + 1, end, hits.size());
        for (size_t i = offset; i < end; ++i)
            out += std::format("{:<6} {}  {}\n", hits[i].kind, hits[i].name, hits[i].detail);
        if (end < hits.size())
            out += std::format("cursor: s{}\n", end);
        return out;
    }

    std::string Server::listTables(const json& args) {
        const int maxResults = num(args, {"maxResults", "max_results", "limit"}, 100, 1, 1000);
        int offset = 0;
        if (auto c = str(args, {"cursor"}); !c.empty())
            offset = cursorOffset(c, 't');

        auto target = resolve(args);
        const auto& cat = catalog(target, offset == 0);
        std::vector<std::pair<std::string, const dearsql::Table*>> rows;
        for (const auto& t : cat.tables)
            rows.emplace_back("table", &t);
        for (const auto& t : cat.views)
            rows.emplace_back("view", &t);

        std::string out;
        if (offset == 0)
            out += std::format("{} tables, {} views in {}\n", cat.tables.size(), cat.views.size(),
                               where(target));
        const size_t end = std::min(rows.size(), static_cast<size_t>(offset + maxResults));
        for (size_t i = offset; i < end; ++i) {
            const auto& [kind, t] = rows[i];
            out += std::format("{}  {}  {} cols", qualified(*t), kind, t->columns.size());
            if (t->sizeBytes >= 0)
                out += "  " + dearsql::formatByteSize(t->sizeBytes);
            out += "\n";
        }
        if (end < rows.size())
            out += std::format("{} more. cursor: t{}\n", rows.size() - end, end);
        return out;
    }

    std::string Server::describeTable(const json& args) {
        std::string name = str(args, {"table", "name", "table_name"});
        if (name.empty())
            throw ToolError("Missing 'table'.");
        auto target = resolve(args);

        auto find = [&](const Catalog& cat) -> std::pair<const dearsql::Table*, bool> {
            const std::string want = lower(name);
            for (const auto* list : {&cat.tables, &cat.views}) {
                for (const auto& t : *list) {
                    if (lower(t.name) == want || lower(qualified(t)) == want)
                        return {&t, list == &cat.views};
                }
            }
            return {nullptr, false};
        };
        auto [found, isView] = find(catalog(target));
        if (!found)
            std::tie(found, isView) = find(catalog(target, true)); // created since the cache
        std::string note;
        if (!found) {
            auto close = suggest(target, name);
            if (close.empty())
                throw ToolError("No table '" + name + "' in " + where(target) +
                                ". Try search_schema.");
            // one clearly-best candidate: show it rather than bounce the agent
            const int top = fuzzy::score(name, close[0].substr(close[0].rfind('.') + 1));
            if (close.size() == 1 || top >= 800) {
                note = std::format("(no '{}'; showing closest match {})\n", name, close[0]);
                name = close[0];
                std::tie(found, isView) = find(catalog(target));
            }
            if (!found) {
                std::string list;
                for (const auto& c : close)
                    list += (list.empty() ? "" : ", ") + c;
                throw ToolError("No table '" + name + "'. Did you mean: " + list + "?");
            }
        }

        // describe gives indexes and fks the listing may skip
        dearsql::Table t = *found;
        if (!isView) {
            try {
                auto handle = target.db;
                if (!t.schema.empty() && hasSchemas(handle->type()))
                    if (auto s = handle->schema(t.schema))
                        handle = s;
                auto full = handle->describeTable(t.name);
                if (!full.columns.empty())
                    t = std::move(full);
                if (t.schema.empty())
                    t.schema = found->schema;
            } catch (const std::exception&) {
                // keep the catalog entry
            }
        }

        std::string out = note;
        out += std::format("{}  {}", qualified(t), isView ? "view" : "table");
        if (t.sizeBytes >= 0)
            out += " · " + dearsql::formatByteSize(t.sizeBytes);
        if (!t.comment.empty())
            out += " · " + t.comment;
        out += "\n";
        size_t width = 4;
        for (const auto& c : t.columns)
            width = std::max(width, std::min<size_t>(c.name.size(), 32));
        std::map<std::string, const dearsql::ForeignKey*> fkByColumn;
        for (const auto& fk : t.foreignKeys)
            fkByColumn[fk.sourceColumn] = &fk;
        for (const auto& c : t.columns) {
            std::string line = std::format("  {:<{}}  {}", c.name, width, c.type);
            if (c.isPrimaryKey)
                line += "  PK";
            if (c.isNotNull && !c.isPrimaryKey)
                line += "  not null";
            if (c.isUnique && !c.isPrimaryKey)
                line += "  unique";
            if (c.isAutoIncrement)
                line += "  auto";
            if (!c.defaultValue.empty() && !dearsql::isNullSentinel(c.defaultValue))
                line += "  default " + cell(c.defaultValue);
            if (auto it = fkByColumn.find(c.name); it != fkByColumn.end())
                line +=
                    std::format("  → {}({})", it->second->targetTable, it->second->targetColumn);
            if (!c.comment.empty())
                line += "  -- " + cell(c.comment);
            out += line + "\n";
        }
        if (!t.indexes.empty()) {
            out += "indexes:\n";
            for (const auto& idx : t.indexes) {
                std::string cols;
                for (const auto& c : idx.columns)
                    cols += (cols.empty() ? "" : ", ") + c;
                out +=
                    std::format("  {} ({}){}{}\n", idx.name, cols, idx.isPrimary ? " primary" : "",
                                idx.isUnique && !idx.isPrimary ? " unique" : "");
            }
        }
        // incoming fks come from the listing, which sees every table
        std::vector<std::string> incoming;
        for (const auto* list : {&catalog(target).tables}) {
            for (const auto& other : *list) {
                for (const auto& fk : other.foreignKeys) {
                    if (lower(fk.targetTable) == lower(t.name) ||
                        lower(fk.targetTable) == lower(qualified(t)))
                        incoming.push_back(
                            std::format("{}({})", qualified(other), fk.sourceColumn));
                }
            }
        }
        if (!incoming.empty()) {
            out += "referenced by: ";
            for (size_t i = 0; i < incoming.size(); ++i)
                out += (i ? ", " : "") + incoming[i];
            out += "\n";
        }
        if (isView && !t.definition.empty()) {
            std::string def = t.definition;
            if (def.size() > 2000)
                def = def.substr(0, 2000) + " …";
            out += "definition: " + def + "\n";
        }
        return out;
    }

    std::string Server::runQuery(const json& args, bool write) {
        const int maxRows = num(args, {"maxRows", "max_rows", "maxResults", "limit"}, 50, 1, 500);

        json page;
        size_t resumeAt = 0; // a cursor whose rows are gone: re-run, start here
        const std::string sql = str(args, {"sql", "query", "statement", "command"});
        const std::string cursor = str(args, {"cursor"});
        if (!cursor.empty() && sql.empty()) {
            // the id hashes connection, database and sql, so it names one result set
            auto it = cursors_.find(cursor);
            if (it == cursors_.end()) {
                cursorOffset(cursor, 'q'); // throws for another tool's cursor
                throw ToolError("Cursor '" + cursor +
                                "' is no longer cached; pass it again with the same sql.");
            }
            page = it->second;
        }
        if (page.is_null() && sql.empty())
            throw ToolError("Missing 'sql'.");
        Target target;
        if (page.is_null()) {
            target = resolve(args);
            const std::string key = target.connection + '\n' + target.database + '\n' + sql;
            // a cursor for another query or database: the agent meant the new one
            if (cursor.starts_with('q') && cursor.ends_with(std::format("-{:08x}", sqlHash(key)))) {
                if (auto it = cursors_.find(cursor); it != cursors_.end())
                    page = it->second;
                else if (!write)
                    resumeAt = cursorOffset(cursor, 'q');
            }
        }
        if (page.is_null()) {
            if (!write && !readOnly(target.db->type(), sql))
                throw ToolError(
                    std::string("Rejected: only read-only statements run here (SELECT, WITH, "
                                "SHOW, EXPLAIN, DESCRIBE, read-only PRAGMA on SQLite, or read "
                                "commands on Redis/MongoDB). ") +
                    (allowWrites_ ? "Use execute_sql for a change the user asked for."
                                  : "Show the user the statement to run themselves."));
            if (write) {
                if (auto lib = host_.connection(target.connection); lib && lib->info().readOnly)
                    throw ToolError(target.connection +
                                    " is a read-only connection in DearSQL. Show the user "
                                    "the statement instead.");
                if (auto why = host_.approveWrite(target.connection, sql); !why.empty())
                    throw ToolError("Not run: " + why);
            }
            const auto result = target.db->execute(sql, FETCH_ROWS + 1);
            if (!result.success()) {
                std::string msg = "Error: " + result.errorMessage();
                if (auto [missing, isTable] = missingName(result.errorMessage());
                    !missing.empty()) {
                    std::vector<std::string> close;
                    if (isTable) {
                        close = suggest(target, missing);
                    } else {
                        // columns of the tables the query names, best first; never the
                        // rejected name itself, which only sends the agent round again
                        // ponytail: word match on the sql, not a parse; falls back to
                        // every table when none is named
                        std::set<std::string> words;
                        static const std::regex word(R"([A-Za-z_][A-Za-z0-9_$]*)");
                        for (std::sregex_iterator it(sql.begin(), sql.end(), word), end; it != end;
                             ++it)
                            words.insert(lower(it->str()));
                        const auto& tables = catalog(target).tables;
                        std::vector<const dearsql::Table*> named, holders;
                        for (const auto& t : tables)
                            if (words.contains(lower(t.name)))
                                named.push_back(&t);
                        if (named.empty())
                            for (const auto& t : tables)
                                named.push_back(&t);
                        std::vector<std::pair<int, std::string>> scored;
                        std::set<std::string> seen;
                        for (const auto* t : named) {
                            for (const auto& col : t->columns) {
                                if (col.name == missing)
                                    continue;
                                // only the case differs (postgres "User_ID"): it needs quotes
                                const std::string name = lower(col.name) == lower(missing)
                                                             ? "\"" + col.name + "\""
                                                             : col.name;
                                if (int s = fuzzy::score(missing, col.name);
                                    s && seen.insert(name).second)
                                    scored.emplace_back(s, name);
                            }
                        }
                        std::ranges::sort(scored, std::greater<>());
                        for (size_t i = 0; i < scored.size() && i < 5; ++i)
                            close.push_back(scored[i].second);
                        for (const auto& t : tables)
                            for (const auto& col : t.columns)
                                if (col.name == missing) {
                                    holders.push_back(&t);
                                    break;
                                }
                        if (!holders.empty()) {
                            msg += "\n" + missing + " is a column of: ";
                            for (size_t i = 0; i < holders.size() && i < 5; ++i)
                                msg += (i ? ", " : "") + qualified(*holders[i]);
                        }
                    }
                    if (!close.empty()) {
                        msg += "\nDid you mean: ";
                        for (size_t i = 0; i < close.size(); ++i)
                            msg += (i ? ", " : "") + close[i];
                        msg += "?";
                    }
                }
                throw ToolError(msg);
            }
            if (write)
                catalogs_.clear(); // ddl may have changed the schema
            // the last statement with rows is the answer; earlier ones only report
            const dearsql::StatementResult* rows = nullptr;
            std::string notes;
            for (const auto& s : result.statements) {
                if (!s.columnNames.empty())
                    rows = &s;
                else if (s.affectedRows > 0)
                    notes += std::format("{} rows affected\n", s.affectedRows);
            }
            for (const auto& m : result.messages)
                notes += m + "\n";
            if (!rows)
                return notes + std::format("OK ({:.0f} ms)", result.executionTimeMs);
            const bool more = rows->tableData.size() > FETCH_ROWS;
            page = {{"sql", sql},
                    {"key", target.connection + '\n' + target.database + '\n' + sql},
                    {"columns", rows->columnNames},
                    {"rows", json::array()},
                    {"offset", 0},
                    {"total", std::min<size_t>(rows->tableData.size(), FETCH_ROWS)},
                    {"more", more},
                    {"ms", result.executionTimeMs},
                    {"notes", notes}};
            for (size_t i = 0; i < rows->tableData.size() && i < FETCH_ROWS; ++i)
                page["rows"].push_back(rows->tableData[i]);
            page["offset"] = std::min<size_t>(resumeAt, page["rows"].size());
        }

        const auto& data = page["rows"];
        const size_t offset = page.value("offset", 0);
        const size_t total = page.value("total", 0);
        const bool more = page.value("more", false);
        std::string out = page.value("notes", "");
        std::string body;
        size_t i = offset;
        for (; i < data.size() && i < offset + maxRows; ++i) {
            std::string line;
            for (size_t c = 0; c < data[i].size(); ++c)
                line += (c ? " | " : "") + cell(data[i][c].get<std::string>());
            if (body.size() + line.size() > MAX_OUTPUT && i > offset)
                break;
            body += line + "\n";
        }
        // ponytail: no COUNT(*) re-run for the true total, it would run a big query twice
        const std::string totalText =
            more ? std::format("{}+ (only the first {} are fetched)", total, total)
                 : std::to_string(total);
        if (offset == 0 && i >= total)
            out += std::format("{} row{} · {:.0f} ms\n", totalText, total == 1 ? "" : "s",
                               page.value("ms", 0.0));
        else
            out += std::format("rows {}-{} of {}\n", offset + 1, i, totalText);
        std::string header;
        for (const auto& c : page["columns"])
            header += (header.empty() ? "" : " | ") + cell(c.get<std::string>());
        out += header + "\n" + body;
        if (i < data.size()) {
            if (i < offset + maxRows)
                out += std::format("page cut at {} rows: output limit of {} KB per page\n",
                                   i - offset, MAX_OUTPUT / 1024);
            page["offset"] = i;
            out += "cursor: " + storeCursor(page.value("key", ""), page) + "\n";
        } else if (more) {
            out += std::format("stopped at {} rows; narrow the query or aggregate.\n", total);
        }
        return out;
    }

} // namespace mcp

namespace mcp {

    std::string Server::s3Tool(const std::string& name, const json& args) {
        // the named S3 connection, else the selected one, else the only one
        std::vector<std::string> s3;
        for (const auto& c : host_.connections()) {
            if (c.type == "s3")
                s3.push_back(c.name);
        }
        std::string conn = str(args, {"connection", "conn"});
        if (!conn.empty()) {
            conn = findConnection(conn);
        } else if (const auto f = host_.focus().first; std::ranges::find(s3, f) != s3.end()) {
            conn = f;
        } else if (s3.size() == 1) {
            conn = s3[0];
        } else {
            throw ToolError(s3.empty() ? "No S3 connections are saved in DearSQL."
                                       : "Pass connection; S3 connections: " +
                                             std::accumulate(std::next(s3.begin()), s3.end(), s3[0],
                                                             [](std::string a, const auto& b) {
                                                                 return a + ", " + b;
                                                             }));
        }
        auto client = host_.s3(conn);
        if (!client && std::ranges::find(s3, conn) != s3.end()) {
            // a saved S3 connection that is not open yet
            if (auto err = host_.connect(conn); !err.empty())
                throw ToolError("Could not open " + conn + ": " + err);
            client = host_.s3(conn);
        }
        if (!client)
            throw ToolError("'" + conn + "' is not an S3 connection. Call list_connections.");

        const std::string bucket = str(args, {"bucket"});
        const std::string key = str(args, {"key", "object", "path_in_bucket"});
        auto need = [](const std::string& v, const char* what) {
            if (v.empty())
                throw ToolError(std::string("Missing '") + what + "'.");
        };

        if (name == "list_objects") {
            std::string out;
            if (bucket.empty()) {
                for (const auto& b : client->listBuckets())
                    out += std::format("{}  {}\n", b.name, b.created);
                return out.empty() ? "No buckets." : out;
            }
            const std::string prefix = str(args, {"prefix", "folder", "path"});
            const auto page = client->listObjects(bucket, prefix, str(args, {"cursor"}));
            for (const auto& p : page.prefixes)
                out += p + "\n";
            for (const auto& o : page.objects)
                out += std::format("{}  {} B  {}\n", o.key, o.size, o.lastModified);
            if (out.empty())
                out = "Nothing under '" + prefix + "'.\n";
            if (!page.nextToken.empty())
                out += "cursor: " + page.nextToken + "\n";
            return out;
        }
        need(bucket, "bucket");
        need(key, "key");
        if (name == "read_object") {
            const auto max =
                static_cast<size_t>(num(args, {"maxBytes", "max_bytes"}, 16384, 1, 1 << 20));
            const std::string data = client->readHead(bucket, key, max);
            if (data.find('\0') != std::string::npos)
                return std::format("Binary file; first {} bytes not shown.", data.size());
            return data.size() == max ? data + std::format("\n[stopped at {} bytes]", max) : data;
        }
        // writes (to the bucket, or a local file for download): the user sees them first
        std::string what;
        const std::string toBucket = str(args, {"toBucket", "to_bucket"}).empty()
                                         ? bucket
                                         : str(args, {"toBucket", "to_bucket"});
        const std::string toKey = str(args, {"toKey", "to_key", "to"});
        const auto mv = args.find("move");
        const bool move =
            mv != args.end() && (mv->is_boolean() ? mv->get<bool>() : mv->dump() == "\"true\"");
        const std::string path = str(args, {"path", "file", "to"});
        if (name == "copy_object") {
            need(toKey, "toKey");
            if (key.ends_with('/') != toKey.ends_with('/'))
                throw ToolError("key and toKey must both be folders (ending '/') or both files.");
            // a move deletes the source after copying: never onto or inside itself
            if (move && toBucket == bucket &&
                (toKey == key || (key.ends_with('/') && toKey.starts_with(key))))
                throw ToolError("Can't move a folder or file into itself.");
            what = std::format("{} s3://{}/{} to s3://{}/{}", move ? "move" : "copy", bucket, key,
                               toBucket, toKey);
        } else if (name == "download_object") {
            need(path, "path");
            what = std::format("download s3://{}/{} to {}", bucket, key, path);
        } else if (name == "delete_object") {
            what = std::format("delete s3://{}/{}{}", bucket, key,
                               key.ends_with('/') ? " and everything in it" : "");
        } else {
            need(path, "path");
            what = std::format("upload {} to s3://{}/{}", path, bucket, key);
        }
        if (auto why = host_.approveWrite(conn, what); !why.empty())
            throw ToolError(why);

        if (name == "copy_object") {
            int n = 1;
            if (key.ends_with('/'))
                n = client->copyPrefix(bucket, key, toBucket, toKey);
            else
                client->copyObject(bucket, key, toBucket, toKey);
            if (move) {
                if (key.ends_with('/'))
                    client->deletePrefix(bucket, key);
                else
                    client->deleteObject(bucket, key);
            }
            return std::format("{} {} object{}.", move ? "Moved" : "Copied", n, n == 1 ? "" : "s");
        }
        if (name == "delete_object") {
            if (!key.ends_with('/')) {
                client->deleteObject(bucket, key);
                return "Deleted " + key + ".";
            }
            const int n = client->deletePrefix(bucket, key);
            return std::format("Deleted {} object{}.", n, n == 1 ? "" : "s");
        }
        if (name == "download_object") {
            client->download(bucket, key, path);
            return "Saved " + path;
        }
        client->upload(bucket, key, path);
        return "Uploaded " + key + ".";
    }

} // namespace mcp

namespace mcp {

    std::string Server::explainQuery(const json& args) {
        const std::string sql = str(args, {"sql", "query", "statement"});
        if (sql.empty())
            throw ToolError("Missing 'sql'.");
        const auto a = args.find("analyze");
        const bool analyze =
            a != args.end() && (a->is_boolean() ? a->get<bool>() : a->dump() == "\"true\"");
        auto target = resolve(args);
        const auto type = target.db->type();
        // EXPLAIN only covers the first statement; anything after it would just run
        if (dearsql::statementRanges(sql, type).size() > 1)
            throw ToolError("Pass one statement.");
        if (analyze && !SqlGuard::isReadOnly(sql))
            throw ToolError("analyze runs the statement, so only for read-only ones. Leave it "
                            "off to see the plan of a change.");
        if (analyze && !dearsql::supportsExplainAnalyze(type))
            throw ToolError("This database has no EXPLAIN ANALYZE; leave analyze off.");
        const auto explain = dearsql::explainSql(type, sql, analyze);
        if (!explain)
            throw ToolError("Query plans are supported on PostgreSQL, MySQL 8 and SQLite.");
        const auto result = target.db->execute(*explain, 0);
        if (!result.success())
            throw ToolError("Error: " + result.errorMessage());
        const auto plan = result.statements.empty()
                              ? std::nullopt
                              : dearsql::parsePlan(type, result.statements.back(), analyze);
        if (!plan)
            throw ToolError("Could not read the plan (MySQL needs 8.0.16+).");

        const auto warnings = dearsql::reviewPlan(*plan);
        auto num = [](double v) {
            return v == static_cast<double>(static_cast<long long>(v))
                       ? std::format("{}", static_cast<long long>(v))
                       : std::format("{:.2f}", v);
        };
        std::string out;
        int lines = 0;
        auto walk = [&](auto&& self, const dearsql::PlanNode& n, int depth) -> void {
            if (++lines > 300) {
                if (lines == 301)
                    out += "(plan cut at 300 nodes)\n";
                return;
            }
            const bool flagged =
                std::ranges::any_of(warnings, [&](const auto& w) { return w.node == &n; });
            out += std::string(static_cast<size_t>(depth) * 2, ' ') + (flagged ? "! " : "- ") +
                   n.operation;
            if (!n.detail.empty())
                out += " " + n.detail;
            if (n.estimatedRows)
                out += "  rows~" + num(*n.estimatedRows);
            if (n.actualRows)
                out += " actual " + num(*n.actualRows);
            if (n.loops && *n.loops != 1)
                out += " x" + num(*n.loops) + " loops";
            if (n.cost)
                out += "  cost " + num(*n.cost);
            if (n.selfTimeMs)
                out += std::format("  {:.3f} ms", *n.selfTimeMs);
            // the conditions an agent needs to pick an index; the rest stays out
            for (const auto& [k, v] : n.properties) {
                if (k.find("Cond") != std::string::npos || k.find("Filter") != std::string::npos ||
                    k == "Sort Key" || k == "Group Key")
                    out += "  " + k + ": " + v;
            }
            out += "\n";
            for (const auto& c : n.children)
                self(self, c, depth + 1);
        };
        walk(walk, plan->root, 0);
        if (plan->executionTimeMs)
            out += std::format("Execution {:.3f} ms\n", *plan->executionTimeMs);
        if (warnings.empty()) {
            out += "No warnings from the plan rules.";
        } else {
            out += "\nWarnings (rule-based hints; ! marks the node):\n";
            for (const auto& w : warnings)
                out += "- [" + w.code + "] " + w.message + "\n";
            out += "explain_finding <code> says how to confirm each one.";
        }
        return out;
    }

} // namespace mcp

namespace mcp {

    std::string Server::reviewIndexes(const json& args) {
        auto target = resolve(args);
        const std::string only = str(args, {"table", "name"});
        std::vector<const dearsql::Table*> picked;
        for (const auto& t : catalog(target).tables) {
            if (only.empty() || lower(t.name) == lower(only) || lower(qualified(t)) == lower(only))
                picked.push_back(&t);
        }
        if (!only.empty() && picked.empty())
            throw ToolError("No table '" + only + "' in " + where(target) + ". Try search_schema.");
        constexpr size_t MAX_TABLES = 200;
        const bool cut = picked.size() > MAX_TABLES;
        if (cut)
            picked.resize(MAX_TABLES);
        // indexes only come from describeTable: one call per table, on its schema
        std::vector<dearsql::Table> tables;
        std::string failed;
        for (const auto* t : picked) {
            const std::string n = qualified(*t);
            try {
                auto handle = target.db;
                if (!t->schema.empty() && hasSchemas(handle->type()))
                    if (auto s = handle->schema(t->schema))
                        handle = s;
                auto full = handle->describeTable(t->name);
                if (full.schema.empty())
                    full.schema = t->schema;
                tables.push_back(std::move(full));
            } catch (const std::exception& e) {
                if (!only.empty())
                    throw ToolError(std::string("Error: ") + e.what());
                failed += (failed.empty() ? "" : ", ") + n;
            }
        }
        const auto findings = dearsql::reviewIndexes(tables);
        std::string out;
        for (const auto& f : findings) {
            out += "- [" + f.code + "] " + f.message + "\n";
            if (!f.suggestion.empty())
                out += "  consider: " + f.suggestion + "\n";
        }
        if (findings.empty())
            out = std::format("No index findings in {} table{}.\n", tables.size(),
                              tables.size() == 1 ? "" : "s");
        else
            out += "explain_finding <code> says how to confirm each one; nothing was changed.\n";
        if (cut)
            out += std::format("Checked the first {} tables; pass table for others.\n", MAX_TABLES);
        if (!failed.empty())
            out += "Could not describe: " + failed + "\n";
        return out;
    }

    std::string Server::explainFinding(const json& args) {
        const std::string code = str(args, {"code", "finding", "name"});
        const auto* d = dearsql::findingDoc(code);
        if (!d) {
            std::string codes;
            for (const auto& doc : dearsql::findingDocs())
                codes += (codes.empty() ? "" : ", ") + std::string(doc.code);
            throw ToolError("Unknown code '" + code + "'. Codes: " + codes);
        }
        return std::format("{}: {}\n\nWhat it means: {}\nHow to confirm: {}\nUsual fix: {}\n"
                           "When to leave it: {}",
                           d->code, d->title, d->meaning, d->check, d->fix, d->ignore);
    }

} // namespace mcp
