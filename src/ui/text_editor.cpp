#include "ui/text_editor.hpp"
#include <algorithm>
#include <cctype>

namespace dearsql {

    namespace {
        bool isWordChar(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        std::string toLower(std::string_view s) {
            std::string r(s);
            std::transform(r.begin(), r.end(), r.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return r;
        }

        bool lessNoCase(const std::string& a, const std::string& b) {
            return std::lexicographical_compare(
                a.begin(), a.end(), b.begin(), b.end(),
                [](char x, char y) { return std::tolower(x) < std::tolower(y); });
        }

        const std::vector<std::string>& sqlKeywords() {
            static const std::vector<std::string> keywords = {
                "SELECT",
                "FROM",
                "WHERE",
                "AND",
                "OR",
                "NOT",
                "IN",
                "IS",
                "NULL",
                "AS",
                "ON",
                "JOIN",
                "LEFT",
                "RIGHT",
                "INNER",
                "OUTER",
                "CROSS",
                "FULL",
                "NATURAL",
                "GROUP",
                "BY",
                "ORDER",
                "ASC",
                "DESC",
                "HAVING",
                "LIMIT",
                "OFFSET",
                "UNION",
                "ALL",
                "DISTINCT",
                "BETWEEN",
                "LIKE",
                "EXISTS",
                "CASE",
                "WHEN",
                "THEN",
                "ELSE",
                "END",
                "BEGIN",
                "COMMIT",
                "ROLLBACK",
                "WITH",
                "RECURSIVE",
                "RETURNING",
                "CASCADE",
                "RESTRICT",
                "REFERENCES",
                "PRIMARY",
                "KEY",
                "FOREIGN",
                "UNIQUE",
                "CHECK",
                "DEFAULT",
                "CONSTRAINT",
                "IF",
                "REPLACE",
                "TEMPORARY",
                "INTO",
                "VALUES",
                "SET",
                "INSERT",
                "UPDATE",
                "DELETE",
                "CREATE",
                "DROP",
                "ALTER",
                "TABLE",
                "INDEX",
                "VIEW",
                "DATABASE",
                "SCHEMA",
                "TRIGGER",
                "FUNCTION",
                "RETURNS",
                "DECLARE",
                "OVER",
                "PARTITION",
                "WINDOW",
                "FILTER",
                "EXCEPT",
                "INTERSECT",
                "TRUE",
                "FALSE",
                "USING",
                "MATERIALIZED",
                "SEQUENCE",
                "TRUNCATE",
                "EXECUTE",
                "RETURN",
                "TRANSACTION",
                "EXPLAIN",
                "ANALYZE",
                "VACUUM",
                "GRANT",
                "REVOKE",
                "COUNT",
                "SUM",
                "AVG",
                "MIN",
                "MAX",
                "COALESCE",
                "NULLIF",
                "CAST",
                "EXTRACT",
                "SUBSTRING",
                "TRIM",
                "UPPER",
                "LOWER",
                "LENGTH",
                "CONCAT",
                "REPLACE",
                "NOW",
                "CURRENT_TIMESTAMP",
                "CURRENT_DATE",
                "CURRENT_TIME",
                "INTERVAL",
                "DATE",
                "TIME",
                "TIMESTAMP",
                "INTEGER",
                "BIGINT",
                "SMALLINT",
                "FLOAT",
                "DOUBLE",
                "DECIMAL",
                "NUMERIC",
                "REAL",
                "CHAR",
                "VARCHAR",
                "TEXT",
                "BOOLEAN",
                "SERIAL",
                "BIGSERIAL",
                "JSON",
                "JSONB",
                "UUID",
                "BYTEA",
                "ARRAY",
                "ILIKE",
                "SIMILAR",
                "ANY",
                "SOME",
                "FETCH",
                "FIRST",
                "NEXT",
                "ROWS",
                "ONLY",
                "LATERAL",
                "GENERATED",
                "ALWAYS",
                "IDENTITY",
                "STORED",
                "VIRTUAL",
            };
            return keywords;
        }

        // the item list matched against the word (and qualifier chain) before the cursor
        std::vector<ListCompletion> match(const ListCompletionRequest& request,
                                          const std::vector<ListCompletion>& items) {
            const std::string word = toLower(request.currentWord);
            std::vector<std::string> parts;
            for (const auto& part : request.qualifierParts)
                parts.push_back(toLower(part));
            const auto qualifiersMatch = [&](const ListCompletion& item) {
                if (parts.empty())
                    return true;
                if (item.qualifiers.size() < parts.size())
                    return false;
                const size_t offset = item.qualifiers.size() - parts.size();
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (toLower(item.qualifiers[offset + i]) != parts[i])
                        return false;
                }
                return true;
            };

            std::vector<std::pair<int, const ListCompletion*>> scored;
            for (const auto& item : items) {
                if (!qualifiersMatch(item) || (!parts.empty() && item.qualifiers.empty()))
                    continue;
                const std::string& insert = item.insertText.empty() ? item.text : item.insertText;
                if (!word.empty() && toLower(insert) == word)
                    continue; // already typed
                const std::string text =
                    toLower(item.matchText.empty() ? item.text : item.matchText);
                const size_t at = word.empty() ? 0 : text.find(word);
                if (at == std::string::npos)
                    continue;
                scored.emplace_back(word.empty() ? 100 : at == 0 ? 200 : 100, &item);
            }
            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first)
                    return a.first > b.first;
                if (a.second->text != b.second->text)
                    return lessNoCase(a.second->text, b.second->text);
                return lessNoCase(a.second->detailText, b.second->detailText);
            });
            std::vector<ListCompletion> out;
            for (const auto& [score, item] : scored)
                out.push_back(*item);
            return out;
        }
    } // namespace

    TextEditor::TextEditor() {
        SetLanguage(Language::SQL);
    }

    void TextEditor::SetLanguage(Language language) {
        SetHighlighter(highlighterFor(language));
        SetLineComment(language == Language::SQL ? "--" : language == Language::Redis ? "#" : "");
        SetIndentWords(language == Language::SQL
                           ? std::vector<std::string>{"begin", "then", "loop", "as", "do"}
                           : std::vector<std::string>{});
        if (language != Language::SQL) {
            SetCompletionProvider(nullptr);
            return;
        }
        // the keywords; the sql editor tab brings its own, schema-aware provider
        std::vector<ListCompletion> items;
        for (const auto& keyword : sqlKeywords())
            items.emplace_back(keyword, CompletionKind::Keyword);
        SetCompletionProvider(listCompletions(std::move(items)));
    }

    TextEditor::CompletionItem TextEditor::Row(std::string text, CompletionKind kind) {
        CompletionItem item{.text = std::move(text)};
        switch (kind) {
        case CompletionKind::Keyword:
            item.icon = "K";
            item.iconColor = Token::Keyword;
            break;
        case CompletionKind::Table:
            item.icon = "T";
            item.iconColor = Token::Function;
            break;
        case CompletionKind::Column:
            item.icon = "C";
            item.iconColor = Token::Text;
            break;
        case CompletionKind::View:
            item.icon = "V";
            item.iconColor = Token::String;
            break;
        case CompletionKind::Sequence:
            item.icon = "S";
            item.iconColor = Token::Number;
            break;
        case CompletionKind::Function:
            item.icon = "F";
            item.iconColor = Token::Type;
            break;
        case CompletionKind::Schema:
            item.icon = "N";
            item.iconColor = Token::Keyword;
            break;
        case CompletionKind::Alias:
            item.icon = "A";
            item.iconColor = Token::Function;
            break;
        }
        return item;
    }

    TextEditor::CompletionProvider listCompletions(std::vector<ListCompletion> items,
                                                   ListCompletionFilter filter) {
        return [items = std::move(items), filter = std::move(filter)](std::string_view content,
                                                                      int cursor, bool forced) {
            int wordStart = cursor;
            while (wordStart > 0 && isWordChar(content[wordStart - 1]))
                --wordStart;
            int qualifierStart = wordStart;
            while (qualifierStart > 0 &&
                   (isWordChar(content[qualifierStart - 1]) || content[qualifierStart - 1] == '.'))
                --qualifierStart;
            ListCompletionRequest request{.content = content,
                                          .cursorIndex = cursor,
                                          .currentWord =
                                              content.substr(wordStart, cursor - wordStart),
                                          .forced = forced};
            std::string part;
            for (const char c : content.substr(qualifierStart, wordStart - qualifierStart)) {
                if (c != '.') {
                    part.push_back(c);
                } else if (!part.empty()) {
                    request.qualifierParts.push_back(std::move(part));
                    part.clear();
                }
            }
            if (!part.empty())
                request.qualifierParts.push_back(std::move(part));

            TextEditor::CompletionResponse response{.replaceStart = wordStart,
                                                    .replaceEnd = cursor};
            if (request.currentWord.empty() && request.qualifierParts.empty() && !forced)
                return response;
            for (auto& found : filter ? filter(request, items) : match(request, items)) {
                if (!found.qualifiers.empty())
                    response.replaceStart = qualifierStart; // the chain is part of the insert
                auto row = TextEditor::Row(std::move(found.text), found.kind);
                row.insertText = std::move(found.insertText);
                row.detailText = std::move(found.detailText);
                response.items.push_back(std::move(row));
            }
            return response;
        };
    }

} // namespace dearsql
