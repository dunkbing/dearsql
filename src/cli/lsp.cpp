// `dearsql --lsp`: dearsql::complete and resolveIdentifierAt behind the
// language server protocol, against the cli's connections
#include "cli/lsp.hpp"

#include "cli/completion_catalog.hpp"
#include "config.hpp"
#include "database/connection_url.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <spdlog/spdlog.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace {

    constexpr auto CATALOG_TTL = std::chrono::minutes(2);

    // lsp CompletionItemKind
    int lspKind(dearsql::CompletionKind kind) {
        switch (kind) {
        case dearsql::CompletionKind::Keyword:
            return 14;
        case dearsql::CompletionKind::Function:
            return 3;
        case dearsql::CompletionKind::Table:
            return 7; // class
        case dearsql::CompletionKind::View:
            return 22; // struct
        case dearsql::CompletionKind::Column:
            return 5; // field
        case dearsql::CompletionKind::Schema:
            return 9; // module
        case dearsql::CompletionKind::Sequence:
            return 6; // variable
        case dearsql::CompletionKind::Alias:
            return 18; // reference
        }
        return 1;
    }

    const char* kindName(dearsql::CompletionKind kind) {
        constexpr const char* names[] = {"keyword", "function", "table",    "view",
                                         "column",  "schema",   "sequence", "alias"};
        return names[static_cast<int>(kind)];
    }

    size_t utf8Length(unsigned char c) {
        if (c < 0x80)
            return 1;
        if ((c >> 5) == 0x6)
            return 2;
        if ((c >> 4) == 0xE)
            return 3;
        if ((c >> 3) == 0x1E)
            return 4;
        return 1; // stray continuation byte
    }

    std::string trim(std::string_view s) {
        const auto b = s.find_first_not_of(" \t\r");
        if (b == std::string_view::npos)
            return "";
        const auto e = s.find_last_not_of(" \t\r");
        return std::string(s.substr(b, e - b + 1));
    }

    std::string expandHome(const std::string& path) {
        if (path.starts_with("~/")) {
            if (const char* home = std::getenv("HOME"))
                return home + path.substr(1);
        }
        return path;
    }

    // file:///a/b%20c.sql -> /a/b c.sql; "" for other schemes
    std::string uriToPath(const std::string& uri) {
        if (!uri.starts_with("file://"))
            return "";
        std::string out;
        for (size_t i = 7; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size()) {
                out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += uri[i];
            }
        }
#ifdef _WIN32
        if (out.size() > 2 && out[0] == '/' && out[2] == ':')
            out.erase(0, 1); // file:///C:/x
#endif
        return out;
    }

    // `-- dearsql: <connection>[/<database>]` on the first line; the raw spec
    std::optional<std::string> directive(const std::string& text) {
        std::string line = trim(std::string_view(text).substr(0, text.find('\n')));
        if (!line.starts_with("--"))
            return std::nullopt;
        line = trim(std::string_view(line).substr(2));
        if (!line.starts_with("dearsql:"))
            return std::nullopt;
        auto spec = trim(std::string_view(line).substr(8));
        if (spec.empty())
            return std::nullopt;
        return spec;
    }

    bool hasSchemas(dearsql::DatabaseType t) {
        return t == dearsql::DatabaseType::POSTGRESQL || t == dearsql::DatabaseType::REDSHIFT ||
               t == dearsql::DatabaseType::MSSQL;
    }

    json range(const std::string& text, size_t start, size_t end) {
        const auto [sl, sc] = lspPositionAt(text, start);
        const auto [el, ec] = lspPositionAt(text, end);
        return {{"start", {{"line", sl}, {"character", sc}}},
                {"end", {{"line", el}, {"character", ec}}}};
    }

    std::string qualified(const std::string& schema, const std::string& name) {
        return schema.empty() ? name : schema + "." + name;
    }

    std::string fkTarget(const dearsql::Table& t, const std::string& column) {
        for (const auto& fk : t.foreignKeys) {
            if (fk.sourceColumn == column)
                return fk.targetTable + "(" + fk.targetColumn + ")";
        }
        return "";
    }

    // "integer · primary key · not null · → users(id)"
    std::string columnFacts(const dearsql::Table* t, const dearsql::Column& c) {
        std::string out = c.type.empty() ? "?" : c.type;
        if (c.isPrimaryKey)
            out += " · primary key";
        out += c.isNotNull || c.isPrimaryKey ? " · not null" : " · nullable";
        if (c.isUnique && !c.isPrimaryKey)
            out += " · unique";
        if (t) {
            if (auto fk = fkTarget(*t, c.name); !fk.empty())
                out += " · → " + fk;
        }
        if (!c.defaultValue.empty())
            out += " · default " + c.defaultValue;
        return out;
    }

    std::string hoverText(const dearsql::ResolvedIdentifier& r) {
        using K = dearsql::CompletionKind;
        switch (r.kind) {
        case K::Table:
        case K::View: {
            std::string out = std::format("**{}** `{}`", r.kind == K::View ? "view" : "table",
                                          qualified(r.schema, r.relation));
            if (r.table) {
                if (!r.table->comment.empty())
                    out += "\n\n" + r.table->comment;
                out += "\n";
                for (const auto& c : r.table->columns)
                    out += std::format("\n- `{}` {}", c.name, columnFacts(r.table, c));
            }
            return out;
        }
        case K::Column: {
            std::string out = std::format("**column** `{}.{}`", r.relation, r.column);
            if (r.columnInfo) {
                out += "\n\n" + columnFacts(r.table, *r.columnInfo);
                if (!r.columnInfo->comment.empty())
                    out += "\n\n" + r.columnInfo->comment;
            }
            return out;
        }
        case K::Schema:
            return std::format("**schema** `{}`", r.schema.empty() ? r.relation : r.schema);
        case K::Sequence:
            return std::format("**sequence** `{}`", qualified(r.schema, r.relation));
        case K::Alias:
            return std::format("**{}** `{}`", r.table ? "alias" : "cte / subquery",
                               r.alias.empty() ? r.relation : r.alias);
        default:
            return "";
        }
    }

    // markdown for a table, view or column item, like its hover (sqls does the
    // same). a column is resolved by trying the insertion in place, which sees
    // through aliases and ctes.
    std::string documentation(const dearsql::CompletionCatalog& catalog,
                              const dearsql::CompletionItem& item, const std::string& text,
                              const dearsql::CompletionResult& result, dearsql::DatabaseType type) {
        using K = dearsql::CompletionKind;
        if (item.kind == K::Column) {
            std::string probe = text;
            probe.replace(result.replaceStart, result.replaceEnd - result.replaceStart,
                          item.insertText);
            auto r = dearsql::resolveIdentifierAt(probe, result.replaceStart, catalog, type);
            return r && r->columnInfo ? hoverText(*r) : "";
        }
        if (item.kind != K::Table && item.kind != K::View)
            return "";
        const auto& list = item.kind == K::View ? catalog.views : catalog.tables;
        for (const auto& t : list) {
            if (t.name != item.label || t.schema != item.owner)
                continue;
            dearsql::ResolvedIdentifier r;
            r.kind = item.kind;
            r.schema = t.schema;
            r.relation = t.name;
            r.table = &t;
            return hoverText(r);
        }
        return "";
    }

} // namespace

size_t lspOffsetAt(const std::string& text, int line, int character) {
    size_t i = 0;
    for (int l = 0; l < line; ++l) {
        i = text.find('\n', i);
        if (i == std::string::npos)
            return text.size();
        ++i;
    }
    int units = 0;
    while (i < text.size() && text[i] != '\n' && units < character) {
        const size_t len = utf8Length(static_cast<unsigned char>(text[i]));
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return std::min(i, text.size());
}

std::pair<int, int> lspPositionAt(const std::string& text, size_t offset) {
    offset = std::min(offset, text.size());
    int line = 0;
    int character = 0;
    for (size_t i = 0; i < offset;) {
        if (text[i] == '\n') {
            ++line;
            character = 0;
            ++i;
            continue;
        }
        const size_t len = utf8Length(static_cast<unsigned char>(text[i]));
        character += len == 4 ? 2 : 1;
        i += len;
    }
    return {line, character};
}

LspServer::LspServer(CliConnections& connections, std::string defaultConnection, Send send)
    : connections_(connections), defaultConnection_(std::move(defaultConnection)),
      send_(std::move(send)) {}

bool LspServer::handle(const json& message) {
    if (!message.is_object() || !message.contains("method"))
        return true; // a response to something we sent, or junk
    const std::string method = message.value("method", "");
    const bool isRequest = message.contains("id");
    const json params = message.value("params", json::object());
    auto reply = [&](json result) {
        send_({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", std::move(result)}});
    };
    auto fail = [&](int code, const std::string& text) {
        send_({{"jsonrpc", "2.0"},
               {"id", message["id"]},
               {"error", {{"code", code}, {"message", text}}}});
    };

    try {
        if (method == "initialize") {
            reply(initialize(params));
        } else if (method == "shutdown") {
            shutdown_ = true;
            reply(nullptr);
        } else if (method == "exit") {
            return false;
        } else if (method == "textDocument/didOpen") {
            const auto& doc = params.at("textDocument");
            documents_[doc.at("uri").get<std::string>()] = doc.value("text", "");
        } else if (method == "textDocument/didChange") {
            auto& text = documents_[params.at("textDocument").at("uri").get<std::string>()];
            for (const auto& change : params.value("contentChanges", json::array())) {
                if (!change.contains("range")) {
                    text = change.value("text", "");
                    continue;
                }
                // we ask for full sync; apply a ranged edit anyway
                const auto& r = change["range"];
                const size_t a = lspOffsetAt(text, r["start"]["line"], r["start"]["character"]);
                const size_t b = lspOffsetAt(text, r["end"]["line"], r["end"]["character"]);
                text.replace(a, std::max(a, b) - a, change.value("text", ""));
            }
        } else if (method == "textDocument/didClose") {
            documents_.erase(params.at("textDocument").at("uri").get<std::string>());
        } else if (method == "textDocument/completion") {
            reply(completion(params));
        } else if (method == "textDocument/hover") {
            reply(hover(params));
        } else if (method == "workspace/executeCommand") {
            reply(executeCommand(params));
        } else if (isRequest) {
            fail(-32601, "method not found: " + method);
        }
        // other notifications ($/cancelRequest, initialized, ...) need nothing
    } catch (const std::exception& e) {
        spdlog::warn("lsp {}: {}", method, e.what());
        if (isRequest)
            fail(-32603, e.what());
    }
    return true;
}

json LspServer::initialize(const json& params) {
    const auto opts = params.value("initializationOptions", json::object());
    if (opts.is_object()) {
        initOptions_.connection = expandHome(opts.value("connection", ""));
        initOptions_.database = opts.value("database", "");
    }
    return {{"capabilities",
             {{"textDocumentSync", 1}, // full
              {"completionProvider", {{"triggerCharacters", {"."}}, {"resolveProvider", false}}},
              {"hoverProvider", true},
              {"executeCommandProvider",
               {{"commands",
                 {"showConnections", "switchConnections", "showDatabases", "switchDatabase",
                  "showTables"}}}}}},
            {"serverInfo", {{"name", "dearsql"}, {"version", APP_VERSION}}}};
}

// the per-file comment wins over the server's defaults (command line, then
// initializationOptions)
std::optional<LspServer::Target> LspServer::targetFor(const std::string& uri,
                                                      const std::string& text) const {
    if (auto spec = directive(text)) {
        std::string s = expandHome(*spec);
        // a relative file path is relative to the document
        const auto dir = std::filesystem::path(uriToPath(uri)).parent_path();
        auto resolve = [&](const std::string& p) -> std::string {
            std::error_code ec;
            if (connections_.find(p) || looksLikeConnectionUrl(p))
                return p;
            const std::filesystem::path path(p);
            if (std::filesystem::exists(path, ec))
                return p;
            if (path.is_relative() && !dir.empty() && std::filesystem::exists(dir / path, ec))
                return (dir / path).string();
            return "";
        };
        if (auto whole = resolve(s); !whole.empty())
            return Target{whole, ""};
        if (auto slash = s.rfind('/'); slash != std::string::npos && slash > 0) {
            auto conn = resolve(s.substr(0, slash));
            return Target{conn.empty() ? s.substr(0, slash) : conn, s.substr(slash + 1)};
        }
        return Target{s, ""};
    }
    return serverTarget();
}

// a switchConnections / switchDatabase choice, else the command line, else
// initializationOptions
std::optional<LspServer::Target> LspServer::serverTarget() const {
    if (switched_)
        return switched_;
    if (!defaultConnection_.empty()) {
        const bool sameAsOptions =
            initOptions_.connection.empty() || initOptions_.connection == defaultConnection_;
        return Target{defaultConnection_, sameAsOptions ? initOptions_.database : ""};
    }
    if (!initOptions_.connection.empty())
        return initOptions_;
    return std::nullopt;
}

const LspServer::Catalog* LspServer::catalogFor(const std::string& uri, const std::string& text) {
    auto target = targetFor(uri, text);
    return target ? catalogFor(*target) : nullptr;
}

const LspServer::Catalog* LspServer::catalogFor(const Target& target) {
    const std::string key = target.connection + "\n" + target.database;
    const auto now = std::chrono::steady_clock::now();
    if (auto it = catalogs_.find(key);
        it != catalogs_.end() && now - it->second.loaded < CATALOG_TTL)
        return &it->second;

    Catalog c;
    c.loaded = now;
    std::string error;
    const auto name = connections_.add(target.connection, error);
    if (name.empty()) {
        c.error = error;
    } else if (auto err = connections_.open(name); !err.empty()) {
        c.type = connections_.find(name)->info.type;
        c.error = "could not open " + name + ": " + err;
    } else {
        auto conn = connections_.connection(name);
        c.type = conn->type();
        try {
            using T = dearsql::DatabaseType;
            if (c.type != T::REDIS && c.type != T::MONGODB) {
                std::string db = target.database;
                std::string schema;
                if (auto dot = db.find('.'); dot != std::string::npos && hasSchemas(c.type)) {
                    schema = db.substr(dot + 1);
                    db = db.substr(0, dot);
                }
                auto handle = conn->database(db);
                if (handle && !schema.empty())
                    handle = handle->schema(schema);
                if (!handle)
                    throw std::runtime_error("no database '" + target.database + "' on " + name);
                c.catalog = loadCompletionCatalog(handle);
                spdlog::info("lsp: {} tables, {} views from {}", c.catalog.tables.size(),
                             c.catalog.views.size(), name);
            }
        } catch (const std::exception& e) {
            c.error = name + ": " + e.what();
        }
    }
    if (!c.error.empty()) {
        spdlog::warn("lsp: {}", c.error);
        warnOnce("DearSQL: " + c.error + ". Completing keywords only.");
    }
    return &(catalogs_[key] = std::move(c));
}

void LspServer::warnOnce(const std::string& message) {
    if (!warned_.insert(message).second)
        return;
    send_({{"jsonrpc", "2.0"},
           {"method", "window/showMessage"},
           {"params", {{"type", 2}, {"message", message}}}});
}

json LspServer::completion(const json& params) {
    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    const auto doc = documents_.find(uri);
    if (doc == documents_.end())
        return json::array();
    const std::string& text = doc->second;
    const int line = params.at("position").at("line");
    const size_t cursor = lspOffsetAt(text, line, params.at("position").at("character"));
    // typing the connection comment itself: don't try to connect to half a name
    if (line == 0 && directive(text))
        return json::array();

    static const dearsql::CompletionCatalog empty;
    const Catalog* cat = catalogFor(uri, text);
    const auto& catalog = cat ? cat->catalog : empty;
    const auto type = cat ? cat->type : dearsql::DatabaseType::POSTGRESQL;
    const auto result = dearsql::complete(text, cursor, catalog, type);

    // what the user typed so far: a quoted start filters on the quoted text
    const std::string typed = text.substr(result.replaceStart, cursor - result.replaceStart);
    const bool quoted = !typed.empty() && (typed[0] == '"' || typed[0] == '`' || typed[0] == '[');
    const json editRange = range(text, result.replaceStart, result.replaceEnd);
    json items = json::array();
    for (size_t i = 0; i < result.items.size(); ++i) {
        const auto& item = result.items[i];
        std::string detail = item.detail;
        if (!item.owner.empty())
            detail += (detail.empty() ? "" : " · ") + item.owner;
        if (detail.empty())
            detail = kindName(item.kind);
        json it = {{"label", item.label},
                   {"kind", lspKind(item.kind)},
                   {"sortText", std::format("{:05}", i)},
                   {"filterText", quoted ? item.insertText : item.label},
                   {"insertTextFormat", 1},
                   {"textEdit", {{"range", editRange}, {"newText", item.insertText}}}};
        if (!detail.empty())
            it["detail"] = detail;
        if (auto doc = documentation(catalog, item, text, result, type); !doc.empty())
            it["documentation"] = {{"kind", "markdown"}, {"value", std::move(doc)}};
        items.push_back(std::move(it));
    }
    return {{"isIncomplete", false}, {"items", std::move(items)}};
}

json LspServer::hover(const json& params) {
    const auto uri = params.at("textDocument").at("uri").get<std::string>();
    const auto doc = documents_.find(uri);
    if (doc == documents_.end())
        return nullptr;
    const std::string& text = doc->second;
    const size_t offset =
        lspOffsetAt(text, params.at("position").at("line"), params.at("position").at("character"));
    const Catalog* cat = catalogFor(uri, text);
    if (!cat || !cat->error.empty())
        return nullptr;
    auto resolved = dearsql::resolveIdentifierAt(text, offset, cat->catalog, cat->type);
    if (!resolved)
        return nullptr;
    const auto value = hoverText(*resolved);
    if (value.empty())
        return nullptr;
    return {{"contents", {{"kind", "markdown"}, {"value", value}}},
            {"range", range(text, resolved->start, resolved->end)}};
}

// base protocol: `Content-Length: N\r\n\r\n` + N bytes of json, both ways
int runLsp(CliConnections& connections, const std::string& defaultConnection) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (isatty(fileno(stdin))) {
        std::cerr << "dearsql language server: waiting for an editor on stdin/stdout.\n"
                     "Point your editor's LSP client at `dearsql --lsp` instead; see\n"
                     "https://dearsql.dev/docs/command-line. Ctrl-C to quit.\n";
    }
    auto write = [](const json& msg) {
        const auto body = msg.dump(-1, ' ', false, json::error_handler_t::replace);
        std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
    };
    LspServer server(connections, defaultConnection, write);
    while (true) {
        size_t length = 0;
        bool sawHeader = false;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty()) {
                if (sawHeader)
                    break;
                continue;
            }
            sawHeader = true;
            constexpr std::string_view key = "content-length:";
            std::string lower = line;
            for (auto& ch : lower)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (lower.starts_with(key))
                length = std::stoul(trim(std::string_view(line).substr(key.size())));
        }
        if (!std::cin)
            return server.exitCode(); // eof: the editor went away
        std::string body(length, '\0');
        if (!std::cin.read(body.data(), static_cast<std::streamsize>(length)))
            return server.exitCode();
        json message;
        try {
            message = json::parse(body);
        } catch (const std::exception& e) {
            spdlog::warn("lsp: bad message: {}", e.what());
            write({{"jsonrpc", "2.0"},
                   {"id", nullptr},
                   {"error", {{"code", -32700}, {"message", e.what()}}}});
            continue;
        }
        if (!server.handle(message))
            return server.exitCode();
    }
}

// sqls's commands, so editor plugins and keymaps written for it carry over:
// show* return plain text, switch* change the server's target (a file's
// `-- dearsql:` comment still wins)
json LspServer::executeCommand(const json& params) {
    const auto command = params.value("command", "");
    const auto args = params.value("arguments", json::array());
    auto argument = [&]() -> std::string {
        if (!args.is_array() || args.empty())
            throw std::runtime_error(command + " needs an argument");
        return args[0].is_string() ? args[0].get<std::string>() : args[0].dump();
    };
    auto current = [&] {
        auto t = serverTarget();
        if (!t)
            throw std::runtime_error("no connection; run switchConnections first");
        return *t;
    };
    auto nameOf = [&](const std::string& spec) {
        std::string error;
        auto name = connections_.add(spec, error);
        if (name.empty())
            throw std::runtime_error(error);
        return name;
    };

    if (command == "showConnections") {
        const auto t = serverTarget();
        const auto active = t ? connections_.find(t->connection) : nullptr;
        std::string out;
        int i = 0;
        for (auto* e : connections_.entries())
            out += std::format("{} {} {}{}\n", ++i, databaseTypeToString(e->info.type),
                               e->info.name, e == active ? " *" : "");
        return out;
    }
    if (command == "switchConnections") {
        auto want = argument();
        const auto entries = connections_.entries();
        // sqls takes a 1-based index as well as a name
        if (!want.empty() &&
            std::ranges::all_of(want, [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            const auto index = std::stoul(want);
            if (index >= 1 && index <= entries.size())
                want = entries[index - 1]->info.name;
        }
        switched_ = Target{nameOf(expandHome(want)), ""};
        spdlog::info("lsp: switched to {}", switched_->connection);
        return nullptr;
    }
    if (command == "switchDatabase") {
        switched_ = Target{current().connection, argument()};
        spdlog::info("lsp: switched to {}/{}", switched_->connection, switched_->database);
        return nullptr;
    }
    if (command == "showDatabases") {
        const auto t = current();
        const auto name = nameOf(t.connection);
        if (auto err = connections_.open(name); !err.empty())
            throw std::runtime_error("could not open " + name + ": " + err);
        auto conn = connections_.connection(name);
        const auto active = t.database.empty() ? conn->info().database : t.database;
        std::string out;
        for (const auto& db : conn->databases())
            out += db->name() + (db->name() == active ? " *" : "") + "\n";
        return out;
    }
    if (command == "showTables") {
        const auto* cat = catalogFor(current());
        if (!cat->error.empty())
            throw std::runtime_error(cat->error);
        std::string out;
        for (const auto* list : {&cat->catalog.tables, &cat->catalog.views}) {
            for (const auto& t : *list)
                out += qualified(t.schema, t.name) + "\n";
        }
        return out;
    }
    throw std::runtime_error("unknown command: " + command);
}
