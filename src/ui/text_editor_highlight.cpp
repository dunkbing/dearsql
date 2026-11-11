#include "ui/text_editor.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <tree_sitter/api.h>
#include <unordered_set>

extern "C" const TSLanguage* tree_sitter_sql();
extern "C" const TSLanguage* tree_sitter_json();

namespace dearsql {

    // Embedded SQL highlight query - maps AST node types to color categories
    // Based on tree-sitter-sql grammar from https://github.com/DerekStride/tree-sitter-sql
    static constexpr const char* SQL_HIGHLIGHT_QUERY = R"scm(
; SQL Keywords
[
  (keyword_select)
  (keyword_from)
  (keyword_where)
  (keyword_and)
  (keyword_or)
  (keyword_not)
  (keyword_in)
  (keyword_is)
  (keyword_null)
  (keyword_as)
  (keyword_on)
  (keyword_join)
  (keyword_left)
  (keyword_right)
  (keyword_inner)
  (keyword_outer)
  (keyword_cross)
  (keyword_full)
  (keyword_natural)
  (keyword_group)
  (keyword_by)
  (keyword_order)
  (keyword_asc)
  (keyword_desc)
  (keyword_having)
  (keyword_limit)
  (keyword_offset)
  (keyword_union)
  (keyword_all)
  (keyword_distinct)
  (keyword_between)
  (keyword_like)
  (keyword_exists)
  (keyword_case)
  (keyword_when)
  (keyword_then)
  (keyword_else)
  (keyword_end)
  (keyword_begin)
  (keyword_commit)
  (keyword_rollback)
  (keyword_with)
  (keyword_recursive)
  (keyword_returning)
  (keyword_conflict)
  (keyword_cascade)
  (keyword_restrict)
  (keyword_references)
  (keyword_primary)
  (keyword_key)
  (keyword_foreign)
  (keyword_unique)
  (keyword_check)
  (keyword_default)
  (keyword_constraint)
  (keyword_if)
  (keyword_replace)
  (keyword_temporary)
  (keyword_temp)
  (keyword_explain)
  (keyword_analyze)
  (keyword_vacuum)
  (keyword_trigger)
  (keyword_procedure)
  (keyword_function)
  (keyword_returns)
  (keyword_declare)
  (keyword_into)
  (keyword_values)
  (keyword_set)
  (keyword_insert)
  (keyword_update)
  (keyword_delete)
  (keyword_create)
  (keyword_drop)
  (keyword_alter)
  (keyword_table)
  (keyword_index)
  (keyword_view)
  (keyword_database)
  (keyword_schema)
  (keyword_do)
  (keyword_nothing)
  (keyword_duplicate)
  (keyword_for)
  (keyword_each)
  (keyword_row)
  (keyword_over)
  (keyword_partition)
  (keyword_window)
  (keyword_filter)
  (keyword_except)
  (keyword_intersect)
  (keyword_true)
  (keyword_false)
  (keyword_use)
  (keyword_show)
  (keyword_tables)
  (keyword_columns)
  (keyword_type)
  (keyword_column)
  (keyword_add)
  (keyword_rename)
  (keyword_to)
  (keyword_only)
  (keyword_using)
  (keyword_materialized)
  (keyword_concurrently)
  (keyword_refresh)
  (keyword_extension)
  (keyword_sequence)
  (keyword_role)
  (keyword_policy)
  (keyword_merge)
  (keyword_matched)
  (keyword_truncate)
  (keyword_copy)
  (keyword_execute)
  (keyword_return)
  (keyword_language)
  (keyword_transaction)
  (keyword_session)
  (keyword_isolation)
  (keyword_level)
  (keyword_read)
  (keyword_write)
  (keyword_serializable)
  (keyword_committed)
  (keyword_uncommitted)
  (keyword_repeatable)
  (keyword_snapshot)
  (keyword_some)
  (keyword_any)
  (keyword_similar)
  (keyword_match)
  (keyword_lateral)
  (keyword_no)
  (keyword_action)
  (keyword_option)
  (keyword_current)
  (keyword_collate)
  (keyword_comment)
  (keyword_owner)
  (keyword_owned)
  (keyword_enable)
  (keyword_disable)
  (keyword_start)
  (keyword_restart)
  (keyword_cache)
  (keyword_cycle)
  (keyword_increment)
  (keyword_minvalue)
  (keyword_maxvalue)
  (keyword_generated)
  (keyword_always)
  (keyword_stored)
  (keyword_virtual)
  (keyword_deferred)
  (keyword_immediate)
  (keyword_deferrable)
  (keyword_initially)
  (keyword_encoding)
  (keyword_tablespace)
  (keyword_storage)
  (keyword_include)
  (keyword_replication)
  (keyword_connection)
  (keyword_password)
  (keyword_valid)
  (keyword_until)
  (keyword_admin)
  (keyword_input)
  (keyword_new)
  (keyword_old)
  (keyword_referencing)
  (keyword_instead)
  (keyword_before)
  (keyword_after)
  (keyword_each)
  (keyword_parallel)
  (keyword_safe)
  (keyword_unsafe)
  (keyword_restricted)
  (keyword_cost)
  (keyword_rows)
  (keyword_volatile)
  (keyword_stable)
  (keyword_immutable)
  (keyword_strict)
  (keyword_called)
  (keyword_security)
  (keyword_definer)
  (keyword_invoker)
  (keyword_leakproof)
  (keyword_support)
  (keyword_setof)
  (keyword_variadic)
  (keyword_inout)
  (keyword_out)
  (keyword_atomic)
  (keyword_while)
  (keyword_statistics)
  (keyword_public)
  (keyword_logged)
  (keyword_unlogged)
  (keyword_ignore)
] @keyword

; Data types
[
  (keyword_int)
  (keyword_bigint)
  (keyword_smallint)
  (keyword_tinyint)
  (keyword_mediumint)
  (keyword_float)
  (keyword_double)
  (keyword_decimal)
  (keyword_numeric)
  (keyword_real)
  (keyword_precision)
  (keyword_char)
  (keyword_character)
  (keyword_varchar)
  (keyword_nchar)
  (keyword_nvarchar)
  (keyword_text)
  (keyword_binary)
  (keyword_varbinary)
  (keyword_boolean)
  (keyword_date)
  (keyword_datetime)
  (keyword_datetime2)
  (keyword_time)
  (keyword_timestamp)
  (keyword_timestamptz)
  (keyword_interval)
  (keyword_serial)
  (keyword_bigserial)
  (keyword_smallserial)
  (keyword_money)
  (keyword_smallmoney)
  (keyword_smalldatetime)
  (keyword_datetimeoffset)
  (keyword_json)
  (keyword_jsonb)
  (keyword_xml)
  (keyword_uuid)
  (keyword_bytea)
  (keyword_inet)
  (keyword_bit)
  (keyword_enum)
  (keyword_image)
  (keyword_regclass)
  (keyword_regtype)
  (keyword_regproc)
  (keyword_regnamespace)
  (keyword_geometry)
  (keyword_geography)
  (keyword_box2d)
  (keyword_box3d)
  (keyword_varying)
  (keyword_unsigned)
  (keyword_zerofill)
  (keyword_auto_increment)
  (keyword_oid)
  (keyword_name)
  (keyword_string)
  (keyword_object_id)
] @type

; Comments
(comment) @comment
(marginalia) @comment

; Strings
(literal) @string

; Function calls
(invocation
  unit: (object_reference
    name: (identifier) @function))

; Numbers (handled via literal, but we can be more specific)
; The grammar uses (literal) for both strings and numbers

; Star/wildcard
(all_fields) @operator
)scm";

    static constexpr const char* JSON_HIGHLIGHT_QUERY = R"scm(
; object property keys
(pair key: (string) @type)

; string values
(pair value: (string) @string)
(array (string) @string)

; numbers
(number) @number

; booleans and null
[
  (true)
  (false)
  (null)
] @keyword

; comments
(comment) @comment
)scm";

    namespace {
        using Token = mydear::TextEditor::Token;
        using Edit = mydear::TextEditor::Edit;

        const std::unordered_set<std::string>& redisCommands() {
            static const std::unordered_set<std::string> REDIS_COMMANDS = {
                "APPEND",
                "AUTH",
                "BGSAVE",
                "BGREWRITEAOF",
                "BITCOUNT",
                "BITFIELD",
                "BITOP",
                "BITPOS",
                "BLMOVE",
                "BLMPOP",
                "BLPOP",
                "BRPOP",
                "BZMPOP",
                "BZPOPMAX",
                "BZPOPMIN",
                "CLIENT",
                "CLUSTER",
                "COMMAND",
                "CONFIG",
                "COPY",
                "DEBUG",
                "DBSIZE",
                "DECR",
                "DECRBY",
                "DEL",
                "DISCARD",
                "DUMP",
                "ECHO",
                "EVAL",
                "EVALSHA",
                "EXEC",
                "EXISTS",
                "EXPIRE",
                "EXPIREAT",
                "EXPIRETIME",
                "FLUSHALL",
                "FLUSHDB",
                "GEOADD",
                "GEODIST",
                "GEOHASH",
                "GEOPOS",
                "GEOSEARCH",
                "GEOSEARCHSTORE",
                "GET",
                "GETBIT",
                "GETDEL",
                "GETEX",
                "GETRANGE",
                "GETSET",
                "HDEL",
                "HELLO",
                "HEXISTS",
                "HGET",
                "HGETALL",
                "HINCRBY",
                "HINCRBYFLOAT",
                "HKEYS",
                "HLEN",
                "HMGET",
                "HMSET",
                "HRANDFIELD",
                "HSCAN",
                "HSET",
                "HSETNX",
                "HSTRLEN",
                "HVALS",
                "INCR",
                "INCRBY",
                "INCRBYFLOAT",
                "INFO",
                "KEYS",
                "LASTSAVE",
                "LCS",
                "LINDEX",
                "LINSERT",
                "LLEN",
                "LMOVE",
                "LMPOP",
                "LPOP",
                "LPOS",
                "LPUSH",
                "LPUSHX",
                "LRANGE",
                "LREM",
                "LSET",
                "LTRIM",
                "MEMORY",
                "MGET",
                "MIGRATE",
                "MODULE",
                "MONITOR",
                "MOVE",
                "MSET",
                "MSETNX",
                "MULTI",
                "OBJECT",
                "PERSIST",
                "PEXPIRE",
                "PEXPIREAT",
                "PEXPIRETIME",
                "PFADD",
                "PFCOUNT",
                "PFMERGE",
                "PING",
                "PSETEX",
                "PSUBSCRIBE",
                "PTTL",
                "PUBLISH",
                "PUBSUB",
                "PUNSUBSCRIBE",
                "QUIT",
                "RANDOMKEY",
                "READONLY",
                "READWRITE",
                "RENAME",
                "RENAMENX",
                "REPLCONF",
                "REPLICAOF",
                "RESET",
                "RESTORE",
                "ROLE",
                "RPOP",
                "RPOPLPUSH",
                "RPUSH",
                "RPUSHX",
                "SADD",
                "SAVE",
                "SCAN",
                "SCARD",
                "SCRIPT",
                "SDIFF",
                "SDIFFSTORE",
                "SELECT",
                "SET",
                "SETBIT",
                "SETEX",
                "SETNX",
                "SETRANGE",
                "SINTER",
                "SINTERCARD",
                "SINTERSTORE",
                "SLAVEOF",
                "SLOWLOG",
                "SMEMBERS",
                "SMISMEMBER",
                "SMOVE",
                "SORT",
                "SORT_RO",
                "SPOP",
                "SRANDMEMBER",
                "SREM",
                "SSCAN",
                "STRLEN",
                "SUBSCRIBE",
                "SUNION",
                "SUNIONSTORE",
                "SWAPDB",
                "SYNC",
                "TIME",
                "TTL",
                "TYPE",
                "UNLINK",
                "UNSUBSCRIBE",
                "UNWATCH",
                "WAIT",
                "WATCH",
                "XACK",
                "XADD",
                "XAUTOCLAIM",
                "XCLAIM",
                "XDEL",
                "XGROUP",
                "XINFO",
                "XLEN",
                "XPENDING",
                "XRANGE",
                "XREAD",
                "XREADGROUP",
                "XREVRANGE",
                "XSETID",
                "XTRIM",
                "ZADD",
                "ZCARD",
                "ZCOUNT",
                "ZDIFF",
                "ZDIFFSTORE",
                "ZINCRBY",
                "ZINTER",
                "ZINTERCARD",
                "ZINTERSTORE",
                "ZLEXCOUNT",
                "ZMPOP",
                "ZMSCORE",
                "ZPOPMAX",
                "ZPOPMIN",
                "ZRANDMEMBER",
                "ZRANGE",
                "ZRANGEBYLEX",
                "ZRANGEBYSCORE",
                "ZRANGESTORE",
                "ZRANK",
                "ZREM",
                "ZREMRANGEBYLEX",
                "ZREMRANGEBYSCORE",
                "ZREMRANGEBYRANK",
                "ZREVRANGE",
                "ZREVRANGEBYLEX",
                "ZREVRANGEBYSCORE",
                "ZREVRANK",
                "ZSCAN",
                "ZSCORE",
                "ZUNION",
                "ZUNIONSTORE",
                "LOLWUT",
                "LATENCY",
            };
            return REDIS_COMMANDS;
        }

        // the whole text each time: redis scripts are short
        void highlightRedis(std::string_view text, std::span<Token> tokens, const Edit&) {
            std::fill(tokens.begin(), tokens.end(), Token::Text);
            const size_t n = text.size();
            size_t i = 0;
            while (i < n) {
                if (std::isspace(static_cast<unsigned char>(text[i]))) {
                    ++i;
                    continue;
                }
                // comment: # to end of line
                if (text[i] == '#') {
                    while (i < n && text[i] != '\n')
                        tokens[i++] = Token::Comment;
                    continue;
                }
                // string: single or double quoted
                if (text[i] == '"' || text[i] == '\'') {
                    const char q = text[i];
                    tokens[i++] = Token::String;
                    while (i < n && text[i] != q && text[i] != '\n') {
                        if (text[i] == '\\' && i + 1 < n)
                            tokens[i++] = Token::String;
                        tokens[i++] = Token::String;
                    }
                    if (i < n && text[i] == q)
                        tokens[i++] = Token::String;
                    continue;
                }
                // number (including negative)
                if (std::isdigit(static_cast<unsigned char>(text[i])) ||
                    (text[i] == '-' && i + 1 < n &&
                     std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
                    if (text[i] == '-')
                        tokens[i++] = Token::Number;
                    while (i < n &&
                           (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.'))
                        tokens[i++] = Token::Number;
                    continue;
                }
                // word: command or identifier
                if (std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_') {
                    size_t j = i;
                    while (j < n && (std::isalnum(static_cast<unsigned char>(text[j])) ||
                                     text[j] == '_' || text[j] == ':' || text[j] == '.'))
                        ++j;
                    std::string upper(j - i, '\0');
                    std::transform(
                        text.begin() + static_cast<ptrdiff_t>(i),
                        text.begin() + static_cast<ptrdiff_t>(j), upper.begin(),
                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                    if (redisCommands().count(upper))
                        std::fill(tokens.begin() + static_cast<ptrdiff_t>(i),
                                  tokens.begin() + static_cast<ptrdiff_t>(j), Token::Keyword);
                    i = j;
                    continue;
                }
                ++i;
            }
        }

        // compiled once per grammar, shared by every editor (queries are immutable)
        TSQuery* compileQuery(const TSLanguage* lang, const char* source) {
            uint32_t errorOffset = 0;
            TSQueryError errorType{};
            TSQuery* query = ts_query_new(lang, source, static_cast<uint32_t>(strlen(source)),
                                          &errorOffset, &errorType);
            if (!query)
                spdlog::error("tree-sitter query compilation failed at offset {}, error type {}",
                              errorOffset, static_cast<int>(errorType));
            return query;
        }

        Token tokenFor(std::string_view capture) {
            if (capture == "keyword")
                return Token::Keyword;
            if (capture == "string")
                return Token::String;
            if (capture == "number")
                return Token::Number;
            if (capture == "comment")
                return Token::Comment;
            if (capture == "function")
                return Token::Function;
            if (capture == "type")
                return Token::Type;
            if (capture == "operator")
                return Token::Operator;
            return Token::Text;
        }

        // one editor's parser and tree; the editor hands over each edit, so only the edited
        // bytes and what the parse restructured (an opened comment or string reaches far) are
        // recoloured, widened to whole lines
        class TreeSitterHighlighter {
        public:
            TreeSitterHighlighter(const TSLanguage* lang, TSQuery* query)
                : parser_(ts_parser_new()), query_(query) {
                ts_parser_set_language(parser_, lang);
            }
            ~TreeSitterHighlighter() {
                if (tree_)
                    ts_tree_delete(tree_);
                ts_parser_delete(parser_);
            }
            TreeSitterHighlighter(const TreeSitterHighlighter&) = delete;
            TreeSitterHighlighter& operator=(const TreeSitterHighlighter&) = delete;

            void operator()(std::string_view text, std::span<Token> tokens, const Edit& edit) {
                if (!query_)
                    return;
                if (tree_ && edit.start == edit.oldEnd && edit.start == edit.newEnd)
                    return; // the same text: the kept kinds stand
                const bool full =
                    !tree_ || (edit.start == 0 && edit.oldEnd == 0 && edit.newEnd == text.size());
                if (tree_ && !full) {
                    TSInputEdit e{};
                    e.start_byte = static_cast<uint32_t>(edit.start);
                    e.old_end_byte = static_cast<uint32_t>(edit.oldEnd);
                    e.new_end_byte = static_cast<uint32_t>(edit.newEnd);
                    e.start_point = {0, e.start_byte};
                    e.old_end_point = {0, e.old_end_byte};
                    e.new_end_point = {0, e.new_end_byte};
                    ts_tree_edit(tree_, &e);
                }
                TSTree* parsed =
                    ts_parser_parse_string(parser_, full ? nullptr : tree_, text.data(),
                                           static_cast<uint32_t>(text.size()));
                std::vector<std::pair<uint32_t, uint32_t>> ranges;
                if (full || !parsed) {
                    ranges.emplace_back(0, static_cast<uint32_t>(text.size()));
                } else {
                    ranges.emplace_back(edit.start, edit.newEnd);
                    uint32_t count = 0;
                    TSRange* changed = ts_tree_get_changed_ranges(tree_, parsed, &count);
                    for (uint32_t i = 0; i < count; ++i)
                        ranges.emplace_back(changed[i].start_byte, changed[i].end_byte);
                    free(changed);
                }
                if (tree_)
                    ts_tree_delete(tree_);
                tree_ = parsed;
                if (!tree_) {
                    std::fill(tokens.begin(), tokens.end(), Token::Text);
                    return;
                }

                TSQueryCursor* cursor = ts_query_cursor_new();
                const auto size = static_cast<uint32_t>(text.size());
                for (auto [from, to] : ranges) {
                    while (from > 0 && text[from - 1] != '\n')
                        --from;
                    while (to < size && text[to] != '\n')
                        ++to;
                    std::fill(tokens.begin() + from, tokens.begin() + to, Token::Text);
                    ts_query_cursor_set_byte_range(cursor, from, to);
                    ts_query_cursor_exec(cursor, query_, ts_tree_root_node(tree_));
                    TSQueryMatch match;
                    while (ts_query_cursor_next_match(cursor, &match)) {
                        for (uint32_t i = 0; i < match.capture_count; ++i) {
                            uint32_t nameLen = 0;
                            const char* name = ts_query_capture_name_for_id(
                                query_, match.captures[i].index, &nameLen);
                            const Token token = tokenFor(std::string_view(name, nameLen));
                            const TSNode node = match.captures[i].node;
                            // only inside the range: outside it the kept kinds stand
                            const uint32_t a = std::max(ts_node_start_byte(node), from);
                            const uint32_t b = std::min(ts_node_end_byte(node), to);
                            for (uint32_t j = a; j < b; ++j)
                                tokens[j] = token;
                        }
                    }
                }
                ts_query_cursor_delete(cursor);
            }

        private:
            TSParser* parser_;
            TSQuery* query_;
            TSTree* tree_ = nullptr;
        };

        mydear::TextEditor::Highlighter treeSitter(const TSLanguage* lang, TSQuery* query) {
            // std::function copies its target: the parser lives behind a shared pointer
            auto highlighter = std::make_shared<TreeSitterHighlighter>(lang, query);
            return [highlighter](std::string_view text, std::span<Token> tokens, const Edit& edit) {
                (*highlighter)(text, tokens, edit);
            };
        }
    } // namespace

    TextEditor::Highlighter TextEditor::highlighterFor(Language language) {
        switch (language) {
        case Language::SQL: {
            static TSQuery* query = compileQuery(tree_sitter_sql(), SQL_HIGHLIGHT_QUERY);
            return treeSitter(tree_sitter_sql(), query);
        }
        case Language::JSON: {
            static TSQuery* query = compileQuery(tree_sitter_json(), JSON_HIGHLIGHT_QUERY);
            return treeSitter(tree_sitter_json(), query);
        }
        case Language::Redis:
            return highlightRedis;
        case Language::PlainText:
            break;
        }
        return nullptr;
    }

} // namespace dearsql
