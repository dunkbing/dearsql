#include "utils/csv_document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include <sstream>

namespace {

    // windows-1252 bytes 0x80-0x9f; 0 = undefined, kept as latin-1
    constexpr std::array<char32_t, 32> kCp1252 = {
        0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

    void appendUtf8(std::string& out, char32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // next code point of utf-8 text; invalid bytes come back as U+FFFD
    char32_t nextUtf8(const std::string& s, std::size_t& i) {
        const auto b = static_cast<unsigned char>(s[i]);
        int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            ++i;
            return 0xFFFD;
        }
        char32_t cp = len == 1 ? b : b & (0x7F >> len);
        for (int k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) {
                ++i;
                return 0xFFFD;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        i += len;
        return cp;
    }

    bool validUtf8(const std::string& s) {
        for (std::size_t i = 0; i < s.size();) {
            const std::size_t at = i;
            if (nextUtf8(s, i) == 0xFFFD &&
                !(s.compare(at, 3, "\xEF\xBF\xBD") == 0)) // a literal U+FFFD is fine
                return false;
        }
        return true;
    }

    std::string decodeUtf16(const std::string& bytes, std::size_t start, bool littleEndian) {
        std::string out;
        out.reserve(bytes.size() / 2);
        auto unit = [&](std::size_t i) -> char16_t {
            const auto a = static_cast<unsigned char>(bytes[i]);
            const auto b = static_cast<unsigned char>(bytes[i + 1]);
            return static_cast<char16_t>(littleEndian ? (b << 8 | a) : (a << 8 | b));
        };
        for (std::size_t i = start; i + 1 < bytes.size(); i += 2) {
            char32_t cp = unit(i);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < bytes.size()) {
                const char16_t lo = unit(i + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            appendUtf8(out, cp);
        }
        return out;
    }

    std::string decodeCp1252(const std::string& bytes) {
        std::string out;
        out.reserve(bytes.size() + bytes.size() / 8);
        for (char ch : bytes) {
            const auto b = static_cast<unsigned char>(ch);
            char32_t cp = b;
            if (b >= 0x80 && b < 0xA0 && kCp1252[b - 0x80] != 0)
                cp = kCp1252[b - 0x80];
            appendUtf8(out, cp);
        }
        return out;
    }

    // bytes without a bom, in a known encoding
    std::string decodeAs(const std::string& bytes, CsvDialect::Encoding encoding) {
        switch (encoding) {
        case CsvDialect::Encoding::Utf16LE:
            return decodeUtf16(bytes, 0, true);
        case CsvDialect::Encoding::Utf16BE:
            return decodeUtf16(bytes, 0, false);
        case CsvDialect::Encoding::Windows1252:
            return decodeCp1252(bytes);
        case CsvDialect::Encoding::Utf8:
            break;
        }
        return bytes;
    }

    // the whole string as a number; strtod since floating from_chars needs macOS 26
    std::optional<double> parseNumber(const std::string& s) {
        if (s.empty() || std::isspace(static_cast<unsigned char>(s[0])))
            return std::nullopt;
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size() || !std::isfinite(v))
            return std::nullopt;
        return v;
    }

    bool isNumber(const std::string& s) {
        return parseNumber(s).has_value();
    }

    bool needsQuotes(const std::string& field, const CsvDialect& d) {
        switch (d.quoteStyle) {
        case CsvDialect::QuoteStyle::All:
            return true;
        case CsvDialect::QuoteStyle::NonNumeric:
            if (!field.empty() && !isNumber(field))
                return true;
            break;
        case CsvDialect::QuoteStyle::Minimal:
            break;
        }
        return field.find(d.delimiter) != std::string::npos ||
               field.find(d.quote) != std::string::npos ||
               field.find_first_of("\r\n") != std::string::npos;
    }

    // kind of a value, for the consistency check
    enum class Kind { Empty, Integer, Decimal, DecimalComma, Date, Email, Text };

    Kind kindOf(const std::string& v) {
        static const std::regex integer(R"([+-]?\d+)");
        static const std::regex decimal(R"([+-]?(\d+\.\d*|\.\d+)([eE][+-]?\d+)?)");
        static const std::regex decimalComma(R"([+-]?\d+,\d+)");
        static const std::regex date(
            R"(\d{4}-\d{1,2}-\d{1,2}([ T]\d{1,2}:\d{2}(:\d{2})?.*)?|\d{1,2}[./]\d{1,2}[./]\d{2,4})");
        static const std::regex email(R"([^@\s]+@[^@\s]+\.[^@\s]+)");
        if (v.empty())
            return Kind::Empty;
        if (std::regex_match(v, integer))
            return Kind::Integer;
        if (std::regex_match(v, decimal))
            return Kind::Decimal;
        if (std::regex_match(v, decimalComma))
            return Kind::DecimalComma;
        if (std::regex_match(v, date))
            return Kind::Date;
        if (std::regex_match(v, email))
            return Kind::Email;
        return Kind::Text;
    }

    std::string lower(std::string s) {
        std::ranges::transform(s, s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool writeAtomically(const std::string& path, const std::string& bytes, std::string& error) {
        namespace fs = std::filesystem;
        const fs::path target = fs::path(path);
        fs::path tmp = target;
        tmp += ".dearsql-tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                error = "Cannot write " + tmp.string();
                return false;
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.close();
            if (!out) {
                error = "Failed writing " + tmp.string();
                std::error_code ec;
                fs::remove(tmp, ec);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp, target, ec);
        if (ec) {
            error = "Cannot replace " + target.string() + ": " + ec.message();
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

} // namespace

namespace csv {

    std::string decode(const std::string& bytes, CsvDialect& d) {
        d.bom = false;
        if (bytes.starts_with("\xEF\xBB\xBF")) {
            d.encoding = CsvDialect::Encoding::Utf8;
            d.bom = true;
            return bytes.substr(3);
        }
        if (bytes.starts_with("\xFF\xFE")) {
            d.encoding = CsvDialect::Encoding::Utf16LE;
            d.bom = true;
            return decodeUtf16(bytes, 2, true);
        }
        if (bytes.starts_with("\xFE\xFF")) {
            d.encoding = CsvDialect::Encoding::Utf16BE;
            d.bom = true;
            return decodeUtf16(bytes, 2, false);
        }
        if (validUtf8(bytes)) {
            d.encoding = CsvDialect::Encoding::Utf8;
            return bytes;
        }
        d.encoding = CsvDialect::Encoding::Windows1252;
        return decodeCp1252(bytes);
    }

    std::string encode(const std::string& utf8, const CsvDialect& d) {
        std::string out;
        switch (d.encoding) {
        case CsvDialect::Encoding::Utf8:
            if (d.bom)
                out = "\xEF\xBB\xBF";
            out += utf8;
            return out;
        case CsvDialect::Encoding::Utf16LE:
        case CsvDialect::Encoding::Utf16BE: {
            const bool le = d.encoding == CsvDialect::Encoding::Utf16LE;
            out.reserve(utf8.size() * 2 + 2);
            auto put = [&](char16_t u) {
                const char hi = static_cast<char>(u >> 8);
                const char lo = static_cast<char>(u & 0xFF);
                out.push_back(le ? lo : hi);
                out.push_back(le ? hi : lo);
            };
            if (d.bom)
                put(0xFEFF);
            for (std::size_t i = 0; i < utf8.size();) {
                char32_t cp = nextUtf8(utf8, i);
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    put(static_cast<char16_t>(0xD800 + (cp >> 10)));
                    put(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
                } else {
                    put(static_cast<char16_t>(cp));
                }
            }
            return out;
        }
        case CsvDialect::Encoding::Windows1252:
            out.reserve(utf8.size());
            for (std::size_t i = 0; i < utf8.size();) {
                const char32_t cp = nextUtf8(utf8, i);
                if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100)) {
                    out.push_back(static_cast<char>(cp));
                    continue;
                }
                const auto it = std::ranges::find(kCp1252, cp);
                out.push_back(it != kCp1252.end() && cp != 0
                                  ? static_cast<char>(0x80 + (it - kCp1252.begin()))
                                  : '?');
            }
            return out;
        }
        return utf8;
    }

    CsvDialect sniff(const std::string& text) {
        CsvDialect d;
        // line break: the first one in the file
        if (const auto nl = text.find_first_of("\r\n"); nl != std::string::npos) {
            if (text[nl] == '\n')
                d.lineBreak = "\n";
            else
                d.lineBreak = nl + 1 < text.size() && text[nl + 1] == '\n' ? "\r\n" : "\r";
        }
        d.trailingLineBreak = text.empty() || text.back() == '\n' || text.back() == '\r';

        // delimiter: the candidate giving the most rows with the same, >1 field count
        const std::string sample = text.substr(0, 64 * 1024);
        static const std::array<std::string, 6> candidates = {",", ";", "\t", "|", ":", "\xC2\xA6"};
        std::size_t bestScore = 0;
        for (const auto& delim : candidates) {
            CsvDialect probe = d;
            probe.delimiter = delim;
            auto records = parse(sample, probe);
            if (records.size() > 50)
                records.resize(50);
            // the sample may cut the last record short
            if (records.size() > 1 && sample.size() < text.size())
                records.pop_back();
            std::map<std::size_t, std::size_t> counts;
            for (const auto& r : records)
                ++counts[r.size()];
            std::size_t score = 0;
            for (const auto& [fields, rows] : counts)
                if (fields > 1)
                    score = std::max(score, rows * 1000 + fields);
            if (score > bestScore) {
                bestScore = score;
                d.delimiter = delim;
            }
        }

        // quote style: every field of the first lines quoted -> keep quoting all
        std::size_t lines = 0;
        bool allQuoted = true;
        bool anyField = false;
        std::size_t pos = 0;
        while (pos < sample.size() && lines < 20 && allQuoted) {
            std::size_t end = sample.find_first_of("\r\n", pos);
            if (end == std::string::npos)
                end = sample.size();
            std::string_view line(sample.data() + pos, end - pos);
            if (!line.empty()) {
                std::size_t start = 0;
                while (start <= line.size()) {
                    std::size_t next = line.find(d.delimiter, start);
                    if (next == std::string_view::npos)
                        next = line.size();
                    const auto field = line.substr(start, next - start);
                    anyField = true;
                    if (field.size() < 2 || field.front() != '"' || field.back() != '"') {
                        allQuoted = false;
                        break;
                    }
                    start = next + d.delimiter.size();
                }
                ++lines;
            }
            pos = end + 1;
        }
        if (allQuoted && anyField)
            d.quoteStyle = CsvDialect::QuoteStyle::All;
        return d;
    }

    std::vector<std::vector<std::string>> parse(const std::string& text, const CsvDialect& d) {
        std::vector<std::vector<std::string>> records;
        if (text.empty())
            return records;
        const std::string& delim = d.delimiter.empty() ? std::string(",") : d.delimiter;
        std::vector<std::string> record;
        std::string field;
        bool inQuotes = false;
        bool fieldStarted = false;
        const std::size_t n = text.size();

        auto endField = [&] {
            record.push_back(std::move(field));
            field.clear();
            fieldStarted = false;
        };
        auto endRecord = [&] {
            endField();
            records.push_back(std::move(record));
            record.clear();
        };

        for (std::size_t i = 0; i < n; ++i) {
            const char c = text[i];
            if (inQuotes) {
                if (c == d.quote) {
                    if (i + 1 < n && text[i + 1] == d.quote) {
                        field.push_back(c);
                        ++i;
                    } else {
                        inQuotes = false;
                    }
                } else {
                    field.push_back(c);
                }
                continue;
            }
            if (c == d.quote && !fieldStarted && field.empty()) {
                inQuotes = true;
                fieldStarted = true;
            } else if (text.compare(i, delim.size(), delim) == 0) {
                endField();
                i += delim.size() - 1;
            } else if (c == '\r' || c == '\n') {
                if (c == '\r' && i + 1 < n && text[i + 1] == '\n')
                    ++i;
                endRecord();
            } else {
                field.push_back(c);
                fieldStarted = true;
            }
        }
        // no record for the line break that ends the file
        if (fieldStarted || !field.empty() || !record.empty() || inQuotes)
            endRecord();
        return records;
    }

    void appendRecord(std::string& out, const std::vector<std::string>& record,
                      const CsvDialect& d) {
        for (std::size_t i = 0; i < record.size(); ++i) {
            if (i > 0)
                out += d.delimiter;
            const std::string& f = record[i];
            if (!needsQuotes(f, d)) {
                out += f;
                continue;
            }
            out.push_back(d.quote);
            for (char c : f) {
                if (c == d.quote)
                    out.push_back(c);
                out.push_back(c);
            }
            out.push_back(d.quote);
        }
    }

    std::string serialize(const std::vector<std::vector<std::string>>& records,
                          const CsvDialect& d) {
        std::string out;
        std::size_t estimate = 0;
        for (const auto& r : records)
            for (const auto& f : r)
                estimate += f.size() + 1;
        out.reserve(estimate + records.size() * d.lineBreak.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            appendRecord(out, records[i], d);
            if (i + 1 < records.size() || d.trailingLineBreak)
                out += d.lineBreak;
        }
        return out;
    }

    const char* encodingName(CsvDialect::Encoding encoding) {
        switch (encoding) {
        case CsvDialect::Encoding::Utf8:
            return "UTF-8";
        case CsvDialect::Encoding::Utf16LE:
            return "UTF-16 LE";
        case CsvDialect::Encoding::Utf16BE:
            return "UTF-16 BE";
        case CsvDialect::Encoding::Windows1252:
            return "Windows-1252";
        }
        return "UTF-8";
    }

    std::string delimiterName(const std::string& delimiter) {
        if (delimiter == "\t")
            return "Tab";
        if (delimiter == ",")
            return "Comma";
        if (delimiter == ";")
            return "Semicolon";
        if (delimiter == "|")
            return "Pipe";
        if (delimiter == ":")
            return "Colon";
        return delimiter;
    }

} // namespace csv

bool CsvDocument::load(const std::string& path, std::string& error,
                       const std::optional<CsvDialect>& dialect) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        error = "Cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    CsvDialect detected;
    const std::string text = csv::decode(buffer.str(), detected);
    if (dialect) {
        dialect_ = *dialect;
        if (dialect_.encoding == detected.encoding) {
            applyRecords(csv::parse(text, dialect_));
        } else {
            // a chosen encoding re-decodes the bytes behind any bom
            std::string raw = buffer.str();
            if (detected.bom)
                raw.erase(0, detected.encoding == CsvDialect::Encoding::Utf8 ? 3 : 2);
            applyRecords(csv::parse(decodeAs(raw, dialect_.encoding), dialect_));
        }
    } else {
        dialect_ = csv::sniff(text);
        dialect_.encoding = detected.encoding;
        dialect_.bom = detected.bom;
        applyRecords(csv::parse(text, dialect_));
    }

    undo_.clear();
    redo_.clear();
    state_ = savedState_ = ++nextState_;
    return true;
}

void CsvDocument::applyRecords(std::vector<std::vector<std::string>> records) {
    std::size_t width = 0;
    for (const auto& r : records)
        width = std::max(width, r.size());
    raggedRows_ = 0;
    for (auto& r : records) {
        if (r.size() != width)
            ++raggedRows_;
        r.resize(width);
    }

    headers_.clear();
    if (dialect_.hasHeader && !records.empty()) {
        headers_ = std::move(records.front());
        records.erase(records.begin());
    } else {
        for (std::size_t i = 0; i < width; ++i)
            headers_.push_back(std::format("Column {}", i + 1));
    }
    rows_ = std::move(records);
    flags_.assign(rows_.size(), false);
}

std::vector<std::vector<std::string>> CsvDocument::records(const std::vector<int>& rows,
                                                           bool all) const {
    std::vector<std::vector<std::string>> out;
    out.reserve((all ? rows_.size() : rows.size()) + 1);
    if (dialect_.hasHeader)
        out.push_back(headers_);
    if (all) {
        out.insert(out.end(), rows_.begin(), rows_.end());
    } else {
        for (int r : rows)
            if (r >= 0 && r < rowCount())
                out.push_back(rows_[r]);
    }
    return out;
}

bool CsvDocument::save(const std::string& path, std::string& error) {
    if (!writeAtomically(path, csv::encode(toText(), dialect_), error))
        return false;
    savedState_ = state_;
    return true;
}

bool CsvDocument::saveRows(const std::string& path, const std::vector<int>& rows,
                           std::string& error) const {
    CsvDialect d = dialect_;
    d.trailingLineBreak = true;
    return writeAtomically(path, csv::encode(csv::serialize(records(rows, false), d), d), error);
}

std::string CsvDocument::toText() const {
    return csv::serialize(records({}, true), dialect_);
}

void CsvDocument::setDialect(const CsvDialect& dialect) {
    CsvDialect next = dialect;
    next.hasHeader = dialect_.hasHeader;
    if (next == dialect_)
        return;
    // a format change is its own undo step and makes the file unsaved
    UndoStep step;
    step.dialect = dialect_;
    pushStep(std::move(step));
    dialect_ = next;
}

void CsvDocument::padRow(std::vector<std::string>& row) const {
    row.resize(headers_.size());
}

CsvDocument::Snapshot CsvDocument::snapshot() const {
    return {headers_, rows_, flags_, dialect_};
}

void CsvDocument::restore(Snapshot s) {
    headers_ = std::move(s.headers);
    rows_ = std::move(s.rows);
    flags_ = std::move(s.flags);
    dialect_ = s.dialect;
}
void CsvDocument::pushStep(UndoStep step) {
    step.state = state_;
    undo_.push_back(std::move(step));
    if (undo_.size() > kMaxUndo)
        undo_.pop_front();
    redo_.clear();
    state_ = ++nextState_;
}

void CsvDocument::pushSnapshot() {
    UndoStep step;
    step.snapshot = snapshot();
    pushStep(std::move(step));
}

CsvDocument::UndoStep CsvDocument::apply(UndoStep step) {
    UndoStep inverse;
    inverse.state = state_;
    if (step.snapshot) {
        inverse.snapshot = snapshot();
        restore(std::move(*step.snapshot));
    } else if (step.dialect) {
        inverse.dialect = dialect_;
        dialect_ = *step.dialect;
    } else {
        inverse.row = step.row;
        inverse.col = step.col;
        inverse.value = std::move(rows_[step.row][step.col]);
        rows_[step.row][step.col] = std::move(step.value);
    }
    state_ = step.state;
    return inverse;
}

bool CsvDocument::undo() {
    if (undo_.empty())
        return false;
    UndoStep step = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(apply(std::move(step)));
    return true;
}

bool CsvDocument::redo() {
    if (redo_.empty())
        return false;
    UndoStep step = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(apply(std::move(step)));
    return true;
}

void CsvDocument::setCell(int row, int col, const std::string& value) {
    if (row < 0 || row >= rowCount() || col < 0 || col >= columnCount())
        return;
    if (rows_[row][col] == value)
        return;
    UndoStep step;
    step.row = row;
    step.col = col;
    step.value = rows_[row][col];
    pushStep(std::move(step));
    rows_[row][col] = value;
}

void CsvDocument::setRange(int row, int col, const std::vector<std::vector<std::string>>& values) {
    if (values.empty() || row < 0 || col < 0)
        return;
    if (values.size() == 1 && values[0].size() == 1) {
        setCell(row, col, values[0][0]);
        return;
    }
    pushSnapshot();
    std::size_t width = 0;
    for (const auto& v : values)
        width = std::max(width, v.size());
    while (columnCount() < col + static_cast<int>(width)) {
        headers_.push_back(dialect_.hasHeader ? std::string{}
                                              : std::format("Column {}", headers_.size() + 1));
        for (auto& r : rows_)
            padRow(r);
    }
    while (rowCount() < row + static_cast<int>(values.size())) {
        rows_.emplace_back(headers_.size());
        flags_.push_back(false);
    }
    for (std::size_t r = 0; r < values.size(); ++r)
        for (std::size_t c = 0; c < values[r].size(); ++c)
            rows_[row + r][col + c] = values[r][c];
}

void CsvDocument::insertRows(int at, int count) {
    at = std::clamp(at, 0, rowCount());
    pushSnapshot();
    rows_.insert(rows_.begin() + at, count, std::vector<std::string>(headers_.size()));
    flags_.insert(flags_.begin() + at, count, false);
}

void CsvDocument::deleteRows(std::vector<int> rows) {
    std::ranges::sort(rows);
    const auto [first, last] = std::ranges::unique(rows);
    rows.erase(first, last);
    std::erase_if(rows, [this](int r) { return r < 0 || r >= rowCount(); });
    if (rows.empty())
        return;
    pushSnapshot();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        rows_.erase(rows_.begin() + *it);
        flags_.erase(flags_.begin() + *it);
    }
}

void CsvDocument::insertColumn(int at, const std::string& name) {
    at = std::clamp(at, 0, columnCount());
    pushSnapshot();
    headers_.insert(headers_.begin() + at,
                    !name.empty() || dialect_.hasHeader ? name : std::format("Column {}", at + 1));
    for (auto& r : rows_)
        r.insert(r.begin() + at, std::string{});
}

void CsvDocument::deleteColumns(int first, int last) {
    first = std::max(first, 0);
    last = std::min(last, columnCount() - 1);
    if (first > last)
        return;
    pushSnapshot();
    headers_.erase(headers_.begin() + first, headers_.begin() + last + 1);
    for (auto& r : rows_)
        r.erase(r.begin() + first, r.begin() + last + 1);
}

bool CsvDocument::moveColumns(int first, int last, int delta) {
    if (first < 0 || last >= columnCount() || first > last)
        return false;
    if ((delta < 0 && first == 0) || (delta > 0 && last == columnCount() - 1))
        return false;
    pushSnapshot();
    auto rotate = [&](auto& v) {
        if (delta < 0)
            std::rotate(v.begin() + first - 1, v.begin() + first, v.begin() + last + 1);
        else
            std::rotate(v.begin() + first, v.begin() + last + 1, v.begin() + last + 2);
    };
    rotate(headers_);
    for (auto& r : rows_)
        rotate(r);
    return true;
}

void CsvDocument::renameColumn(int col, const std::string& name) {
    if (col < 0 || col >= columnCount() || headers_[col] == name)
        return;
    pushSnapshot();
    headers_[col] = name;
}

void CsvDocument::sortBy(int col, bool ascending) {
    if (col < 0 || col >= columnCount() || rows_.size() < 2)
        return;
    pushSnapshot();
    // flags travel with their rows
    std::vector<std::size_t> order(rows_.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::vector<std::optional<double>> numbers(rows_.size());
    for (std::size_t i = 0; i < rows_.size(); ++i)
        numbers[i] = parseNumber(rows_[i][col]);
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        const std::string& x = rows_[a][col];
        const std::string& y = rows_[b][col];
        // empty cells last either way
        if (x.empty() != y.empty())
            return y.empty();
        const auto& nx = numbers[a];
        const auto& ny = numbers[b];
        // numbers before text, numbers by value
        if (nx.has_value() != ny.has_value())
            return ascending ? nx.has_value() : ny.has_value();
        if (nx)
            return ascending ? *nx < *ny : *ny < *nx;
        return ascending ? x < y : y < x;
    });
    std::vector<std::vector<std::string>> sorted;
    std::vector<bool> flags;
    sorted.reserve(rows_.size());
    flags.reserve(rows_.size());
    for (std::size_t i : order) {
        sorted.push_back(std::move(rows_[i]));
        flags.push_back(flags_[i]);
    }
    rows_ = std::move(sorted);
    flags_ = std::move(flags);
}

CsvDocument::Matcher::Matcher(const FindOptions& find) : find_(find) {
    needle_ = find.matchCase ? find.text : lower(find.text);
    if (!find.regex || find.text.empty())
        return;
    try {
        auto flags = std::regex::ECMAScript;
        if (!find.matchCase)
            flags |= std::regex::icase;
        regex_.emplace(find.text, flags);
    } catch (const std::regex_error&) {
        valid_ = false;
    }
}

bool CsvDocument::Matcher::operator()(const std::string& value) const {
    if (find_.text.empty() || !valid_)
        return false;
    if (regex_)
        return find_.wholeCell ? std::regex_match(value, *regex_)
                               : std::regex_search(value, *regex_);
    if (find_.matchCase)
        return find_.wholeCell ? value == needle_ : value.find(needle_) != std::string::npos;
    const std::string v = lower(value);
    return find_.wholeCell ? v == needle_ : v.find(needle_) != std::string::npos;
}

std::string CsvDocument::Matcher::replace(const std::string& value,
                                          const std::string& replacement) const {
    if (!(*this)(value))
        return value;
    if (regex_)
        return std::regex_replace(value, *regex_, replacement);
    if (find_.wholeCell)
        return replacement;
    // plain text: every occurrence, case-insensitively when asked
    const std::string hay = find_.matchCase ? value : lower(value);
    std::string out;
    std::size_t from = 0;
    for (std::size_t at = hay.find(needle_); at != std::string::npos;
         at = hay.find(needle_, from)) {
        out.append(value, from, at - from);
        out += replacement;
        from = at + needle_.size();
    }
    out.append(value, from);
    return out;
}

std::optional<std::pair<int, int>> CsvDocument::findNext(const FindOptions& find, int row, int col,
                                                         bool forward,
                                                         const std::vector<int>& rows) const {
    const Matcher matches(find);
    if (find.text.empty() || !matches.valid() || rows_.empty() || headers_.empty())
        return std::nullopt;

    const int viewRows = rows.empty() ? rowCount() : static_cast<int>(rows.size());
    const int cols = columnCount();
    if (viewRows == 0)
        return std::nullopt;
    // position in the view, not the document
    int vr = 0;
    if (rows.empty()) {
        vr = std::clamp(row, 0, viewRows - 1);
    } else {
        const auto it = std::ranges::lower_bound(rows, row);
        vr = it == rows.end() ? viewRows - 1 : static_cast<int>(it - rows.begin());
    }
    const long total = static_cast<long>(viewRows) * cols;
    long pos = static_cast<long>(vr) * cols + std::clamp(col, 0, cols - 1);
    for (long step = 0; step < total; ++step) {
        pos = forward ? (pos + 1) % total : (pos - 1 + total) % total;
        const int r = static_cast<int>(pos / cols);
        const int c = static_cast<int>(pos % cols);
        if (find.column >= 0 && c != find.column)
            continue;
        const int docRow = rows.empty() ? r : rows[r];
        if (matches(rows_[docRow][c]))
            return std::make_pair(docRow, c);
    }
    return std::nullopt;
}

int CsvDocument::replaceAll(const FindOptions& find, const std::string& replacement) {
    const Matcher matches(find);
    if (find.text.empty() || !matches.valid())
        return 0;
    // collect first so a search that finds nothing leaves no undo step
    std::vector<std::pair<int, int>> hits;
    for (int r = 0; r < rowCount(); ++r)
        for (int c = 0; c < columnCount(); ++c)
            if ((find.column < 0 || c == find.column) && matches(rows_[r][c]))
                hits.emplace_back(r, c);
    if (hits.empty())
        return 0;
    pushSnapshot();
    for (const auto& [r, c] : hits)
        rows_[r][c] = matches.replace(rows_[r][c], replacement);
    return static_cast<int>(hits.size());
}

void CsvDocument::setCells(const std::vector<CellValue>& cells) {
    std::vector<const CellValue*> changed;
    for (const auto& cell : cells)
        if (cell.row >= 0 && cell.row < rowCount() && cell.col >= 0 && cell.col < columnCount() &&
            rows_[cell.row][cell.col] != cell.value)
            changed.push_back(&cell);
    if (changed.empty())
        return;
    if (changed.size() == 1) {
        setCell(changed[0]->row, changed[0]->col, changed[0]->value);
        return;
    }
    pushSnapshot();
    for (const auto* cell : changed)
        rows_[cell->row][cell->col] = cell->value;
}

int CsvDocument::splitColumn(int col, const std::string& separator) {
    if (col < 0 || col >= columnCount() || separator.empty())
        return 0;
    std::vector<std::vector<std::string>> parts(rows_.size());
    std::size_t width = 1;
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        const std::string& v = rows_[r][col];
        std::size_t from = 0;
        for (std::size_t at = v.find(separator); at != std::string::npos;
             at = v.find(separator, from)) {
            parts[r].push_back(v.substr(from, at - from));
            from = at + separator.size();
        }
        parts[r].push_back(v.substr(from));
        width = std::max(width, parts[r].size());
    }
    if (width < 2)
        return 0;
    pushSnapshot();
    const std::string base = headers_[col];
    std::vector<std::string> names;
    for (std::size_t i = 0; i < width; ++i)
        names.push_back(std::format("{} {}", base, i + 1));
    headers_.erase(headers_.begin() + col);
    headers_.insert(headers_.begin() + col, names.begin(), names.end());
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        parts[r].resize(width);
        rows_[r].erase(rows_[r].begin() + col);
        rows_[r].insert(rows_[r].begin() + col, parts[r].begin(), parts[r].end());
    }
    return static_cast<int>(width);
}

void CsvDocument::mergeColumns(int first, int last, const std::string& separator) {
    first = std::max(first, 0);
    last = std::min(last, columnCount() - 1);
    if (first >= last)
        return;
    pushSnapshot();
    for (auto& r : rows_) {
        std::string merged = r[first];
        for (int c = first + 1; c <= last; ++c) {
            if (!r[c].empty()) {
                if (!merged.empty())
                    merged += separator;
                merged += r[c];
            }
        }
        r[first] = std::move(merged);
        r.erase(r.begin() + first + 1, r.begin() + last + 1);
    }
    headers_.erase(headers_.begin() + first + 1, headers_.begin() + last + 1);
}

void CsvDocument::setHasHeader(bool hasHeader) {
    if (hasHeader == dialect_.hasHeader)
        return;
    pushSnapshot();
    if (hasHeader) {
        if (rows_.empty()) {
            headers_.assign(headers_.size(), std::string{});
        } else {
            headers_ = std::move(rows_.front());
            rows_.erase(rows_.begin());
            flags_.erase(flags_.begin());
        }
    } else {
        rows_.insert(rows_.begin(), headers_);
        flags_.insert(flags_.begin(), false);
        for (std::size_t i = 0; i < headers_.size(); ++i)
            headers_[i] = std::format("Column {}", i + 1);
    }
    dialect_.hasHeader = hasHeader;
}

void CsvDocument::setFlags(const std::vector<int>& rows, bool flagged) {
    for (int r : rows)
        if (r >= 0 && r < static_cast<int>(flags_.size()))
            flags_[r] = flagged;
}

void CsvDocument::invertFlags() {
    flags_.flip();
}

void CsvDocument::clearFlags() {
    std::fill(flags_.begin(), flags_.end(), false);
}

std::vector<int> CsvDocument::flaggedRows() const {
    std::vector<int> out;
    for (std::size_t i = 0; i < flags_.size(); ++i)
        if (flags_[i])
            out.push_back(static_cast<int>(i));
    return out;
}

int CsvDocument::flaggedCount() const {
    return static_cast<int>(std::ranges::count(flags_, true));
}

int CsvDocument::flagInconsistent(int col) {
    if (col < 0 || col >= columnCount())
        return 0;
    std::vector<Kind> kinds;
    kinds.reserve(rows_.size());
    std::map<Kind, int> counts;
    for (const auto& r : rows_) {
        kinds.push_back(kindOf(r[col]));
        if (kinds.back() != Kind::Empty)
            ++counts[kinds.back()];
    }
    if (counts.empty())
        return 0;
    const Kind common =
        std::ranges::max_element(counts, {}, &std::pair<const Kind, int>::second)->first;
    int flagged = 0;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        // integers and decimals mix in a numeric column
        auto numeric = [](Kind k) { return k == Kind::Integer || k == Kind::Decimal; };
        const bool fits =
            kinds[i] == common || kinds[i] == Kind::Empty || (numeric(common) && numeric(kinds[i]));
        if (!fits) {
            flags_[i] = true;
            ++flagged;
        }
    }
    return flagged;
}

std::string CsvDocument::toJson(const std::vector<int>& rows) const {
    auto out = nlohmann::ordered_json::array();
    auto addRow = [&](const std::vector<std::string>& r) {
        nlohmann::ordered_json obj = nlohmann::ordered_json::object();
        for (std::size_t c = 0; c < headers_.size(); ++c) {
            std::string key = headers_[c].empty() ? std::format("Column {}", c + 1) : headers_[c];
            obj[key] = r[c];
        }
        out.push_back(std::move(obj));
    };
    if (rows.empty()) {
        for (const auto& r : rows_)
            addRow(r);
    } else {
        for (int r : rows)
            if (r >= 0 && r < rowCount())
                addRow(rows_[r]);
    }
    return out.dump(2) + "\n";
}

std::vector<std::string> CsvDocument::splitFile(const std::string& path, int rowsPerFile,
                                                std::string& error) const {
    std::vector<std::string> written;
    if (rowsPerFile < 1) {
        error = "Rows per file must be at least 1";
        return written;
    }
    const auto p = std::filesystem::path(path);
    const std::string stem = p.stem().string();
    const std::string ext = p.has_extension() ? p.extension().string() : ".csv";
    for (int start = 0, part = 1; start < rowCount(); start += rowsPerFile, ++part) {
        std::vector<int> rows;
        for (int r = start; r < std::min(rowCount(), start + rowsPerFile); ++r)
            rows.push_back(r);
        const auto out = (p.parent_path() / std::format("{}_{}{}", stem, part, ext)).string();
        if (!saveRows(out, rows, error))
            return written;
        written.push_back(out);
    }
    return written;
}
