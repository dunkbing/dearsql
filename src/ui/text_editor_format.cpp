#include "ui/text_editor.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_sql();

namespace dearsql {

    namespace {

        std::string nodeText(TSNode node, const std::string& src) {
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            if (start >= src.size())
                return "";
            return src.substr(start,
                              std::min(static_cast<size_t>(end - start), src.size() - start));
        }

        std::string toUpper(const std::string& s) {
            std::string r = s;
            std::transform(r.begin(), r.end(), r.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            return r;
        }

        void appendIndent(std::string& out, int indent) {
            for (int i = 0; i < indent; ++i)
                out += "    ";
        }

        void appendNewlineIndent(std::string& out, int indent) {
            // Trim trailing spaces before newline
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            out += '\n';
            appendIndent(out, indent);
        }

        bool startsWith(const char* str, const char* prefix) {
            return strncmp(str, prefix, strlen(prefix)) == 0;
        }

        bool isClauseNode(const char* type) {
            return strcmp(type, "from") == 0 || strcmp(type, "where") == 0 ||
                   strcmp(type, "group_by") == 0 || strcmp(type, "order_by") == 0 ||
                   strcmp(type, "having") == 0 || strcmp(type, "limit") == 0 ||
                   strcmp(type, "offset") == 0;
        }

        bool isJoinNode(const char* type) {
            return strcmp(type, "join") == 0 || strcmp(type, "cross_join") == 0 ||
                   strcmp(type, "lateral_join") == 0 || strcmp(type, "lateral_cross_join") == 0;
        }

        bool isStatementNode(const char* type) {
            return strcmp(type, "select") == 0 || strcmp(type, "insert") == 0 ||
                   strcmp(type, "update") == 0 || strcmp(type, "delete") == 0 ||
                   strcmp(type, "create_table") == 0 || strcmp(type, "create_view") == 0 ||
                   strcmp(type, "create_index") == 0 || strcmp(type, "alter_table") == 0 ||
                   strcmp(type, "drop_table") == 0 || strcmp(type, "drop_view") == 0 ||
                   strcmp(type, "drop_index") == 0;
        }

        // Forward declaration
        void formatNode(TSNode node, const std::string& src, int baseIndent, std::string& out,
                        bool& needsSpace);

        void emitLeaf(TSNode node, const std::string& src, std::string& out, bool& needsSpace) {
            const char* type = ts_node_type(node);
            std::string text = nodeText(node, src);

            if (startsWith(type, "keyword_")) {
                if (needsSpace && !out.empty() && out.back() != '\n' && out.back() != ' ' &&
                    out.back() != '(')
                    out += ' ';
                out += toUpper(text);
                needsSpace = true;
            } else if (strcmp(type, "comment") == 0) {
                if (needsSpace && !out.empty() && out.back() != '\n')
                    out += ' ';
                out += text;
                needsSpace = true;
            } else {
                // Punctuation or identifier/literal
                if (text == "," || text == ";") {
                    out += text;
                    needsSpace = true;
                } else if (text == "(") {
                    if (needsSpace && !out.empty() && out.back() != ' ' && out.back() != '\n')
                        out += ' ';
                    out += '(';
                    needsSpace = false;
                } else if (text == ")") {
                    out += ')';
                    needsSpace = true;
                } else if (text == ".") {
                    out += '.';
                    needsSpace = false;
                } else if (text == "=") {
                    if (!out.empty() && out.back() != ' ')
                        out += ' ';
                    out += '=';
                    needsSpace = true;
                } else if (text == "<>" || text == "!=" || text == "<=" || text == ">=" ||
                           text == "<" || text == ">" || text == "||" || text == "+" ||
                           text == "-" || text == "*" || text == "/" || text == "%") {
                    if (!out.empty() && out.back() != ' ')
                        out += ' ';
                    out += text;
                    needsSpace = true;
                } else {
                    if (needsSpace && !out.empty() && out.back() != '\n' && out.back() != ' ' &&
                        out.back() != '(' && out.back() != '.')
                        out += ' ';
                    out += text;
                    needsSpace = true;
                }
            }
        }

        void formatSelectExpression(TSNode node, const std::string& src, int indent,
                                    std::string& out, bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            bool firstItem = true;
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);
                std::string text = nodeText(child, src);

                if (text == ",") {
                    out += ',';
                    appendNewlineIndent(out, indent + 1);
                    needsSpace = false;
                    firstItem = false;
                } else {
                    if (!firstItem && needsSpace && !out.empty() && out.back() != '\n' &&
                        out.back() != ' ')
                        out += ' ';
                    formatNode(child, src, indent, out, needsSpace);
                    firstItem = false;
                }
            }
        }

        void formatFrom(TSNode node, const std::string& src, int baseIndent, std::string& out,
                        bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);

                if (isClauseNode(childType)) {
                    appendNewlineIndent(out, baseIndent);
                    needsSpace = false;
                    formatNode(child, src, baseIndent, out, needsSpace);
                } else if (isJoinNode(childType)) {
                    appendNewlineIndent(out, baseIndent);
                    needsSpace = false;
                    formatNode(child, src, baseIndent, out, needsSpace);
                } else if (strcmp(childType, "keyword_from") == 0) {
                    // The FROM keyword itself
                    emitLeaf(child, src, out, needsSpace);
                } else {
                    // Table references, etc.
                    formatNode(child, src, baseIndent, out, needsSpace);
                }
            }
        }

        void formatWhere(TSNode node, const std::string& src, int baseIndent, std::string& out,
                         bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);

                if (strcmp(childType, "keyword_and") == 0 || strcmp(childType, "keyword_or") == 0) {
                    appendNewlineIndent(out, baseIndent + 1);
                    needsSpace = false;
                    emitLeaf(child, src, out, needsSpace);
                } else {
                    formatNode(child, src, baseIndent, out, needsSpace);
                }
            }
        }

        void formatBinaryExpression(TSNode node, const std::string& src, int baseIndent,
                                    std::string& out, bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);

                if (strcmp(childType, "keyword_and") == 0 || strcmp(childType, "keyword_or") == 0) {
                    appendNewlineIndent(out, baseIndent + 1);
                    needsSpace = false;
                    emitLeaf(child, src, out, needsSpace);
                } else {
                    formatNode(child, src, baseIndent, out, needsSpace);
                }
            }
        }

        void formatCase(TSNode node, const std::string& src, int baseIndent, std::string& out,
                        bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);

                if (strcmp(childType, "keyword_case") == 0) {
                    emitLeaf(child, src, out, needsSpace);
                } else if (strcmp(childType, "keyword_when") == 0 ||
                           strcmp(childType, "keyword_else") == 0) {
                    appendNewlineIndent(out, baseIndent + 1);
                    needsSpace = false;
                    emitLeaf(child, src, out, needsSpace);
                } else if (strcmp(childType, "keyword_end") == 0) {
                    appendNewlineIndent(out, baseIndent);
                    needsSpace = false;
                    emitLeaf(child, src, out, needsSpace);
                } else {
                    formatNode(child, src, baseIndent + 1, out, needsSpace);
                }
            }
        }

        void formatInvocation(TSNode node, const std::string& src, int baseIndent, std::string& out,
                              bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);
                std::string text = nodeText(child, src);

                if (strcmp(childType, "object_reference") == 0) {
                    // Function name — uppercase it
                    if (needsSpace && !out.empty() && out.back() != '\n' && out.back() != ' ' &&
                        out.back() != '(' && out.back() != '.')
                        out += ' ';
                    out += toUpper(text);
                    needsSpace = false; // No space before '('
                } else if (text == "(") {
                    out += '(';
                    needsSpace = false;
                } else if (text == ")") {
                    out += ')';
                    needsSpace = true;
                } else if (text == ",") {
                    out += ',';
                    needsSpace = true;
                } else {
                    // Arguments — emit inline
                    if (needsSpace && !out.empty() && out.back() != '(' && out.back() != '\n')
                        out += ' ';
                    formatNode(child, src, baseIndent, out, needsSpace);
                }
            }
        }

        void formatSubquery(TSNode node, const std::string& src, int baseIndent, std::string& out,
                            bool& needsSpace) {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                std::string text = nodeText(child, src);

                if (text == "(") {
                    if (!out.empty() && out.back() != ' ' && out.back() != '\n')
                        out += ' ';
                    out += '(';
                    appendNewlineIndent(out, baseIndent + 1);
                    needsSpace = false;
                } else if (text == ")") {
                    appendNewlineIndent(out, baseIndent);
                    out += ')';
                    needsSpace = true;
                } else {
                    formatNode(child, src, baseIndent + 1, out, needsSpace);
                }
            }
        }

        void formatNode(TSNode node, const std::string& src, int baseIndent, std::string& out,
                        bool& needsSpace) {
            if (ts_node_is_null(node))
                return;

            const char* type = ts_node_type(node);
            uint32_t childCount = ts_node_child_count(node);

            // Leaf node
            if (childCount == 0) {
                emitLeaf(node, src, out, needsSpace);
                return;
            }

            // Special handling for specific node types
            if (strcmp(type, "select_expression") == 0) {
                formatSelectExpression(node, src, baseIndent, out, needsSpace);
                return;
            }
            if (strcmp(type, "from") == 0) {
                formatFrom(node, src, baseIndent, out, needsSpace);
                return;
            }
            if (strcmp(type, "where") == 0 || strcmp(type, "having") == 0) {
                formatWhere(node, src, baseIndent, out, needsSpace);
                return;
            }
            if (strcmp(type, "binary_expression") == 0) {
                formatBinaryExpression(node, src, baseIndent, out, needsSpace);
                return;
            }
            if (strcmp(type, "case") == 0) {
                formatCase(node, src, baseIndent, out, needsSpace);
                return;
            }
            if (strcmp(type, "invocation") == 0) {
                formatInvocation(node, src, baseIndent, out, needsSpace);
                return;
            }
            if (strcmp(type, "all_fields") == 0) {
                // `*` in SELECT * or COUNT(*) — emit as identifier, not operator
                if (needsSpace && !out.empty() && out.back() != '\n' && out.back() != ' ' &&
                    out.back() != '(' && out.back() != '.')
                    out += ' ';
                out += '*';
                needsSpace = true;
                return;
            }
            if (strcmp(type, "subquery") == 0) {
                formatSubquery(node, src, baseIndent, out, needsSpace);
                return;
            }

            // Generic: recurse into children
            for (uint32_t i = 0; i < childCount; ++i) {
                TSNode child = ts_node_child(node, i);
                const char* childType = ts_node_type(child);
                std::string text = nodeText(child, src);

                // Clause-level children get newlines
                if (isClauseNode(childType) || isJoinNode(childType)) {
                    appendNewlineIndent(out, baseIndent);
                    needsSpace = false;
                    formatNode(child, src, baseIndent, out, needsSpace);
                } else if (isStatementNode(childType) && !out.empty() && out.back() != '\n') {
                    // Nested statement (e.g., in CTE or INSERT ... SELECT)
                    appendNewlineIndent(out, baseIndent);
                    needsSpace = false;
                    formatNode(child, src, baseIndent, out, needsSpace);
                } else if (text == ";") {
                    out += ';';
                    out += '\n';
                    needsSpace = false;
                } else if (text == ",") {
                    // Commas in generic context (e.g., column_definitions, list)
                    out += ',';
                    needsSpace = true;
                } else {
                    formatNode(child, src, baseIndent, out, needsSpace);
                }
            }
        }

    } // anonymous namespace

    std::string TextEditor::FormatSQL(const std::string& sql) {
        if (sql.empty())
            return sql;

        // Create temporary parser
        TSParser* parser = ts_parser_new();
        const TSLanguage* lang = tree_sitter_sql();
        ts_parser_set_language(parser, lang);

        TSTree* tree =
            ts_parser_parse_string(parser, nullptr, sql.c_str(), static_cast<uint32_t>(sql.size()));

        if (!tree) {
            ts_parser_delete(parser);
            return sql; // Parse failed, return original
        }

        TSNode root = ts_tree_root_node(tree);
        std::string result;
        result.reserve(sql.size() * 2);
        bool needsSpace = false;

        // Walk top-level children (statements)
        uint32_t stmtCount = ts_node_child_count(root);
        for (uint32_t i = 0; i < stmtCount; ++i) {
            TSNode child = ts_node_child(root, i);
            if (ts_node_child_count(child) == 0 && nodeText(child, sql) == ";") {
                while (!result.empty() && result.back() == ' ')
                    result.pop_back();
                result += ';';
                if (result.back() != '\n')
                    result += '\n';
                needsSpace = false;
                continue;
            }
            if (!result.empty() && result.back() != '\n')
                result += '\n';
            formatNode(child, sql, 0, result, needsSpace);
        }

        // Trim trailing whitespace, ensure single trailing newline
        while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())))
            result.pop_back();
        if (!result.empty())
            result += '\n';

        ts_tree_delete(tree);
        ts_parser_delete(parser);

        return result;
    }

} // namespace dearsql
