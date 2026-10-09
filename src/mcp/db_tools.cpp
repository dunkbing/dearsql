#include "mcp/db_tools.hpp"

#include "mcp/fuzzy.hpp"
#include "utils/sql_guard.hpp"
#include <algorithm>
#include <format>
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
            return t == POSTGRESQL || t == REDSHIFT || t == MSSQL;
        }

        std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string qualified(const dearsql::Table& t) {
            return t.schema.empty() ? t.name : t.schema + "." + t.name;
        }

        std::string cell(const std::string& v) {
            if (dearsql::isNullSentinel(v))
                return "NULL";
            if (dearsql::isBoolSentinel(v))
                return dearsql::boolSentinelValue(v) ? "true" : "false";
            std::string out;
            out.reserve(std::min(v.size(), MAX_CELL + 16));
            for (char c : v) {
                if (out.size() >= MAX_CELL) {
                    out += std::format("…(+{} chars)", v.size() - MAX_CELL);
                    break;
                }
                out += c == '\n'   ? std::string("\\n")
                       : c == '\t' ? std::string(" ")
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
              {"description", "Connection name. Default: the one selected in DearSQL, or the only "
                              "open one."}}},
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
               "- run_query: read-only SQL (SELECT/WITH/SHOW/EXPLAIN), or a read-only command on "
               "Redis/MongoDB. Returns up to maxRows rows and a cursor for more.\n"
               "- list_tables: the whole map (tables and views with column counts and sizes), "
               "paged. Use when search_schema is not enough.\n"
               "- list_connections: the user's saved connections. Naming one in any tool opens "
               "it; connect opens one explicitly.\n\n"
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

    json Server::handle(const json& req) {
        if (!req.contains("id"))
            return nullptr; // notification
        const std::string method = req.value("method", "");
        json res = {{"jsonrpc", "2.0"}, {"id", req["id"]}};
        const json params = req.value("params", json::object());

        if (method == "initialize") {
            res["result"] = {{"protocolVersion", params.value("protocolVersion", "2025-03-26")},
                             {"capabilities", {{"tools", json::object()}}},
                             {"serverInfo", {{"name", "dearsql"}, {"version", "1.0"}}},
                             {"instructions", instructions()}};
        } else if (method == "ping") {
            res["result"] = json::object();
        } else if (method == "tools/list") {
            json maxResults = {{"type", "integer"}, {"description", "Max results (default 20)."}};
            json cursor = {{"type", "string"},
                           {"description", "Cursor from a previous result, for the next page."}};
            res["result"] = {
                {"tools",
                 json::array(
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
                              "LIMIT. Redis/MongoDB take their read commands instead of SQL.",
                              withTarget({{"sql", {{"type", "string"}}},
                                          {"maxRows",
                                           {{"type", "integer"},
                                            {"description", "Rows per page (default 50)."}}},
                                          {"cursor", cursor}}),
                              {"sql"}),
                      toolDef("list_tables",
                              "Every table and view with column count and size, paged. Prefer "
                              "search_schema when you know roughly what you want.",
                              withTarget({{"maxResults", maxResults}, {"cursor", cursor}})),
                      toolDef("list_connections",
                              "The user's saved connections, which are open, and the databases "
                              "on open ones.",
                              json::object()),
                      toolDef("connect", "Open one of the user's saved connections by name.",
                              {{"name", {{"type", "string"}}}}, {"name"})})}};
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
            if (name == "run_query")
                return textResult(runQuery(args));
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
                if (c.open)
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

        // exact (case-insensitive) or the best close match
        const ConnectionEntry* entry = nullptr;
        int best = 0;
        for (const auto& c : conns) {
            const int s = lower(c.name) == lower(conn) ? 2000 : fuzzy::score(conn, c.name);
            if (s > best) {
                best = s;
                entry = &c;
            }
        }
        if (!entry || best < 400)
            throw ToolError("No connection named '" + conn + "'. Call list_connections.");
        // a connection the agent named (or the user selected) opens on first use
        if (!entry->open) {
            if (auto err = host_.connect(entry->name); !err.empty())
                throw ToolError("Could not open " + entry->name + ": " + err);
        }
        auto lib = host_.connection(entry->name);
        if (!lib)
            throw ToolError("'" + entry->name + "' is not open. Call connect.");

        std::string db = str(args, {"database", "db"});
        std::string schema = str(args, {"schema"});
        if (db.empty() && entry->name == focusConn)
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
            throw ToolError("Could not open database '" + db + "' on " + entry->name + ".");
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
        return {entry->name, label, std::move(handle)};
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

    std::string Server::storeCursor(json state) {
        const std::string id = std::to_string(nextCursor_++);
        cursors_[id] = std::move(state);
        cursorOrder_.push_back(id);
        while (cursorOrder_.size() > MAX_CURSORS) {
            cursors_.erase(cursorOrder_.front());
            cursorOrder_.pop_front();
        }
        return id;
    }

    json Server::takeCursor(const std::string& id) {
        auto it = cursors_.find(id);
        if (it == cursors_.end())
            throw ToolError("Cursor '" + id + "' expired; run the call again without it.");
        return it->second;
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
        out += "* = selected in DearSQL. Closed ones open with connect.";
        return out;
    }

    std::string Server::connect(const json& args) {
        const std::string name = str(args, {"name", "connection"});
        if (name.empty())
            throw ToolError("Missing 'name'.");
        const auto conns = host_.connections();
        const ConnectionEntry* entry = nullptr;
        int best = 0;
        for (const auto& c : conns) {
            const int s = lower(c.name) == lower(name) ? 2000 : fuzzy::score(name, c.name);
            if (s > best) {
                best = s;
                entry = &c;
            }
        }
        if (!entry || best < 400)
            throw ToolError("No connection named '" + name + "'. Call list_connections.");
        if (entry->open)
            return entry->name + " is already open.";
        if (auto err = host_.connect(entry->name); !err.empty())
            throw ToolError("Could not connect to " + entry->name + ": " + err);
        return "Connected to " + entry->name + ".";
    }

    std::string Server::searchSchema(const json& args) {
        const std::string query = str(args, {"query", "pattern", "q", "name"});
        if (query.empty())
            throw ToolError("Missing 'query'.");
        const int maxResults = num(args, {"maxResults", "max_results", "limit"}, 20, 1, 200);
        int offset = 0;
        if (auto c = str(args, {"cursor"}); !c.empty())
            offset = takeCursor(c).value("offset", 0);

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
        // every word must hit the table name or the column name
        auto scoreAll = [&](const std::string& tableName, const std::string& colName) {
            int total = 0;
            for (const auto& w : words) {
                int s = fuzzy::score(w, tableName);
                if (!colName.empty())
                    s = std::max(s, fuzzy::score(w, colName));
                if (s == 0)
                    return 0;
                total += s;
            }
            return total;
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
                        s = s * 8 / 10;
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
                               query, total, target.database);

        std::string out;
        if (offset == 0 && hits[0].kind != "column" &&
            (hits.size() == 1 || hits[0].score >= 1000 || hits[0].score > hits[1].score * 2))
            out += std::format("→ describe_table {} (best match)\n", hits[0].name);
        const size_t end = std::min(hits.size(), static_cast<size_t>(offset + maxResults));
        if (hits.size() > static_cast<size_t>(maxResults) || offset > 0)
            out += std::format("{}-{} of {} matches\n", offset + 1, end, hits.size());
        for (size_t i = offset; i < end; ++i)
            out += std::format("{:<6} {}  {}\n", hits[i].kind, hits[i].name, hits[i].detail);
        if (end < hits.size())
            out += "cursor: " + storeCursor({{"offset", end}}) + "\n";
        return out;
    }

    std::string Server::listTables(const json& args) {
        const int maxResults = num(args, {"maxResults", "max_results", "limit"}, 100, 1, 1000);
        int offset = 0;
        if (auto c = str(args, {"cursor"}); !c.empty())
            offset = takeCursor(c).value("offset", 0);

        auto target = resolve(args);
        const auto& cat = catalog(target, offset == 0);
        std::vector<std::pair<std::string, const dearsql::Table*>> rows;
        for (const auto& t : cat.tables)
            rows.emplace_back("table", &t);
        for (const auto& t : cat.views)
            rows.emplace_back("view", &t);

        std::string out;
        if (offset == 0)
            out += std::format(
                "{} tables, {} views in {} ({})\n", cat.tables.size(), cat.views.size(),
                target.database.empty() ? "database" : target.database, target.connection);
        const size_t end = std::min(rows.size(), static_cast<size_t>(offset + maxResults));
        for (size_t i = offset; i < end; ++i) {
            const auto& [kind, t] = rows[i];
            out += std::format("{}  {}  {} cols", qualified(*t), kind, t->columns.size());
            if (t->sizeBytes >= 0)
                out += "  " + dearsql::formatByteSize(t->sizeBytes);
            out += "\n";
        }
        if (end < rows.size())
            out += std::format("{} more. cursor: {}\n", rows.size() - end,
                               storeCursor({{"offset", end}}));
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
                throw ToolError("No table '" + name + "' in " + target.database +
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

    std::string Server::runQuery(const json& args) {
        const int maxRows = num(args, {"maxRows", "max_rows", "maxResults", "limit"}, 50, 1, 500);

        json page;
        if (auto c = str(args, {"cursor"}); !c.empty()) {
            page = takeCursor(c);
        } else {
            const std::string sql = str(args, {"sql", "query", "statement", "command"});
            if (sql.empty())
                throw ToolError("Missing 'sql'.");
            auto target = resolve(args);
            if (!readOnly(target.db->type(), sql))
                throw ToolError("Rejected: only read-only statements run here (SELECT, WITH, "
                                "SHOW, EXPLAIN, DESCRIBE, or read commands on Redis/MongoDB). "
                                "Show the user the statement to run themselves.");
            const auto result = target.db->execute(sql, FETCH_ROWS + 1);
            if (!result.success()) {
                std::string msg = "Error: " + result.errorMessage();
                if (auto [missing, isTable] = missingName(result.errorMessage());
                    !missing.empty()) {
                    std::vector<std::string> close;
                    if (isTable) {
                        close = suggest(target, missing);
                    } else {
                        // columns of every table, best first
                        std::vector<std::pair<int, std::string>> scored;
                        std::set<std::string> seen;
                        for (const auto& t : catalog(target).tables) {
                            for (const auto& col : t.columns) {
                                if (int s = fuzzy::score(missing, col.name);
                                    s && seen.insert(col.name).second)
                                    scored.emplace_back(s, col.name);
                            }
                        }
                        std::ranges::sort(scored, std::greater<>());
                        for (size_t i = 0; i < scored.size() && i < 5; ++i)
                            close.push_back(scored[i].second);
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
            page = {{"columns", rows->columnNames},
                    {"rows", json::array()},
                    {"offset", 0},
                    {"total", std::min<size_t>(rows->tableData.size(), FETCH_ROWS)},
                    {"more", more},
                    {"ms", result.executionTimeMs},
                    {"notes", notes}};
            for (size_t i = 0; i < rows->tableData.size() && i < FETCH_ROWS; ++i)
                page["rows"].push_back(rows->tableData[i]);
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
        const std::string totalText = more ? std::format("{}+", total) : std::to_string(total);
        if (offset == 0 && i >= total)
            out += std::format("{} row{} · {:.0f} ms\n", totalText, total == 1 ? "" : "s",
                               page.value("ms", 0.0));
        else
            out += std::format("rows {}-{} of {}\n", offset + 1, i, totalText);
        std::string header;
        for (const auto& c : page["columns"])
            header += (header.empty() ? "" : " | ") + c.get<std::string>();
        out += header + "\n" + body;
        if (i < data.size()) {
            page["offset"] = i;
            out += "cursor: " + storeCursor(page) + "\n";
        } else if (more) {
            out += std::format("stopped at {} rows; narrow the query or aggregate.\n", total);
        }
        return out;
    }

} // namespace mcp
