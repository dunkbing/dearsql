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

bool CsvDocument::load(const std::string& path, std::string& error) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        error = "Cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    CsvDialect detected;
    const std::string text = csv::decode(buffer.str(), detected);
    dialect_ = csv::sniff(text);
    dialect_.encoding = detected.encoding;
    dialect_.bom = detected.bom;
    applyRecords(csv::parse(text, dialect_));

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
}

bool CsvDocument::save(const std::string& path, std::string& error) {
    if (!writeAtomically(path, csv::encode(toText(), dialect_), error))
        return false;
    savedState_ = state_;
    return true;
}

std::string CsvDocument::toText() const {
    std::vector<std::vector<std::string>> out;
    out.reserve(rows_.size() + 1);
    if (dialect_.hasHeader && !headers_.empty())
        out.push_back(headers_);
    out.insert(out.end(), rows_.begin(), rows_.end());
    return csv::serialize(out, dialect_);
}

void CsvDocument::padRow(std::vector<std::string>& row) const {
    row.resize(headers_.size());
}

CsvDocument::Snapshot CsvDocument::snapshot() const {
    return {headers_, rows_};
}

void CsvDocument::restore(Snapshot s) {
    headers_ = std::move(s.headers);
    rows_ = std::move(s.rows);
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
    }
    for (std::size_t r = 0; r < values.size(); ++r)
        for (std::size_t c = 0; c < values[r].size(); ++c)
            rows_[row + r][col + c] = values[r][c];
}

void CsvDocument::insertRows(int at, int count) {
    at = std::clamp(at, 0, rowCount());
    pushSnapshot();
    rows_.insert(rows_.begin() + at, count, std::vector<std::string>(headers_.size()));
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
    }
}

void CsvDocument::sortBy(int col, bool ascending) {
    if (col < 0 || col >= columnCount() || rows_.size() < 2)
        return;
    pushSnapshot();
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
    sorted.reserve(rows_.size());
    for (std::size_t i : order) {
        sorted.push_back(std::move(rows_[i]));
    }
    rows_ = std::move(sorted);
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
