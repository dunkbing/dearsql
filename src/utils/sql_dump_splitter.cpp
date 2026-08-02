#include "utils/sql_dump_splitter.hpp"

#include <cctype>
#include <istream>
#include <string_view>

namespace {

    enum class State {
        Normal,
        SingleQuote,
        DoubleQuote,
        Backtick,
        LineComment,
        BlockComment,
        // /*!...*/ -- copied into the statement, but ';' inside it does not end
        // the statement (mysqldump wraps whole CREATE TRIGGER bodies this way).
        ConditionalComment,
    };

    bool isBlank(const std::string& text) {
        return text.find_first_not_of(" \t\r\n") == std::string::npos;
    }

    std::string trim(const std::string& text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // A DELIMITER directive is client-side and must never reach the server. It is
    // only recognized at the start of a statement, which is where mysqldump emits it.
    bool parseDelimiterDirective(const std::string& line, std::string& delimiter) {
        static constexpr std::string_view keyword = "DELIMITER";

        auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos || line.size() - pos < keyword.size()) {
            return false;
        }
        for (size_t i = 0; i < keyword.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(line[pos + i])) != keyword[i]) {
                return false;
            }
        }

        pos += keyword.size();
        if (pos >= line.size() || (line[pos] != ' ' && line[pos] != '\t')) {
            return false;
        }
        pos = line.find_first_not_of(" \t", pos);
        if (pos == std::string::npos) {
            return false;
        }

        const auto end = line.find_last_not_of(" \t\r");
        delimiter = line.substr(pos, end - pos + 1);
        return !delimiter.empty();
    }

    bool matchesAt(const std::string& line, const size_t pos, const std::string& token) {
        return line.compare(pos, token.size(), token) == 0;
    }

} // namespace

bool SqlDumpSplitter::split(
    std::istream& in,
    const std::function<bool(const std::string& statement, bool compound)>& onStatement) {
    std::string delimiter = ";";
    std::string statement;
    State state = State::Normal;
    std::string line;

    const auto emit = [&]() {
        const std::string trimmed = trim(statement);
        statement.clear();
        return trimmed.empty() ? true : onStatement(trimmed, delimiter != ";");
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (state == State::Normal && isBlank(statement)) {
            if (std::string parsed; parseDelimiterDirective(line, parsed)) {
                delimiter = parsed;
                statement.clear();
                continue;
            }
        }

        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];

            switch (state) {
            case State::Normal:
                // Cheap first-character test before the compare: in Normal state
                // this runs on every byte of the dump.
                if (c == delimiter[0] && matchesAt(line, i, delimiter)) {
                    if (!emit()) {
                        return false;
                    }
                    i += delimiter.size() - 1;
                } else if (c == '\'') {
                    state = State::SingleQuote;
                    statement += c;
                } else if (c == '"') {
                    state = State::DoubleQuote;
                    statement += c;
                } else if (c == '`') {
                    state = State::Backtick;
                    statement += c;
                } else if (c == '#') {
                    state = State::LineComment;
                } else if (c == '-' && i + 1 < line.size() && line[i + 1] == '-' &&
                           (i + 2 >= line.size() || line[i + 2] == ' ' || line[i + 2] == '\t')) {
                    state = State::LineComment;
                } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                    if (i + 2 < line.size() && line[i + 2] == '!') {
                        state = State::ConditionalComment;
                        statement += "/*!";
                        i += 2;
                    } else {
                        state = State::BlockComment;
                        ++i;
                    }
                } else {
                    statement += c;
                }
                break;

            case State::SingleQuote:
            case State::DoubleQuote:
            case State::Backtick: {
                const char quote = state == State::SingleQuote   ? '\''
                                   : state == State::DoubleQuote ? '"'
                                                                 : '`';
                statement += c;
                // Backslash escapes apply to strings but not to backtick identifiers.
                if (c == '\\' && state != State::Backtick && i + 1 < line.size()) {
                    statement += line[++i];
                } else if (c == quote) {
                    if (i + 1 < line.size() && line[i + 1] == quote) {
                        statement += line[++i]; // doubled quote, stays in the string
                    } else {
                        state = State::Normal;
                    }
                }
                break;
            }

            case State::LineComment:
                i = line.size();
                break;

            case State::BlockComment:
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    state = State::Normal;
                    ++i;
                    // A comment separates tokens, so dropping it outright would
                    // fuse them: SELECT 1/*c*/UNION must not become SELECT 1UNION.
                    statement += ' ';
                }
                break;

            case State::ConditionalComment:
                statement += c;
                if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                    statement += line[++i];
                    state = State::Normal;
                }
                break;
            }
        }

        if (state == State::LineComment) {
            state = State::Normal;
        }
        if (state != State::BlockComment && !statement.empty()) {
            statement += '\n';
        }
    }

    return emit();
}
