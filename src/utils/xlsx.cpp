#include "utils/xlsx.hpp"
#include "database/db.hpp"
#include <xlsxwriter.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <locale>
#include <sstream>
#include <spdlog/spdlog.h>

namespace {
    std::string lower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    bool isTextType(const std::string& type) {
        const std::string t = lower(type);
        for (const char* k : {"char", "text", "string", "json", "uuid", "enum", "clob", "xml",
                              "inet", "blob", "binary", "bytea", "set"})
            if (t.find(k) != std::string::npos)
                return true;
        return false;
    }

    // zoned timestamps stay text: converting would shift or drop the offset
    bool isDateType(const std::string& type) {
        const std::string t = lower(type);
        return (t.find("date") != std::string::npos || t.find("timestamp") != std::string::npos) &&
               t.find("tz") == std::string::npos && t.find("zone") == std::string::npos &&
               t.find("offset") == std::string::npos;
    }

    // days from 1970-01-01 (howard hinnant's days_from_civil)
    long long daysFromCivil(long long y, unsigned m, unsigned d) {
        y -= m <= 2;
        const long long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<long long>(doe) - 719468;
    }

    bool digits(const std::string& v, size_t pos, size_t n, int& out) {
        if (pos + n > v.size())
            return false;
        const auto [p, ec] = std::from_chars(v.data() + pos, v.data() + pos + n, out);
        return ec == std::errc{} && p == v.data() + pos + n;
    }

    // what xml 1.0 (and so excel) accepts: control characters other than tab and line breaks
    // are dropped; malformed utf-8 (latin1 columns, binary in text), surrogates and
    // U+FFFE/U+FFFF become U+FFFD, since excel calls a file holding them corrupt
    std::string cleanText(const std::string& v, size_t& truncated) {
        constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
        std::string out;
        out.reserve(std::min(v.size(), xlsx::kMaxCellChars));
        const auto byte = [&](size_t i) { return static_cast<unsigned char>(v[i]); };
        for (size_t i = 0; i < v.size();) {
            const unsigned char b = byte(i);
            if (b < 0x80) {
                if (b >= 0x20 || b == '\t' || b == '\n' || b == '\r')
                    out.push_back(static_cast<char>(b));
                ++i;
                continue;
            }
            const size_t len = b >= 0xF0 && b <= 0xF4 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 && b <= 0xDF ? 2 : 0;
            bool ok = len > 0 && b < 0xF5 && i + len <= v.size();
            uint32_t cp = ok ? (b & (0xFF >> (len + 1))) : 0;
            for (size_t k = 1; ok && k < len; ++k) {
                ok = (byte(i + k) & 0xC0) == 0x80;
                cp = (cp << 6) | (byte(i + k) & 0x3F);
            }
            const bool wellFormed = ok; // a whole sequence, even if its code point is refused
            // overlong forms, surrogates, past U+10FFFF, and the two non-characters
            ok = ok && !(len == 3 && cp < 0x800) && !(len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) &&
                 !(cp >= 0xD800 && cp <= 0xDFFF) && cp != 0xFFFE && cp != 0xFFFF;
            if (ok) {
                out.append(v, i, len);
                i += len;
            } else {
                out += kReplacement;
                i += wellFormed ? len : 1;
            }
        }
        if (out.size() > xlsx::kMaxCellChars) {
            size_t end = xlsx::kMaxCellChars;
            while (end > 0 && (static_cast<unsigned char>(out[end]) & 0xC0) == 0x80)
                --end;
            out.resize(end);
            ++truncated;
        }
        return out;
    }
} // namespace

namespace xlsx {

    std::string sheetName(const std::string& wanted, const std::vector<std::string>& taken) {
        std::string base;
        for (const char c : wanted)
            base.push_back(std::string_view("[]:*?/\\").find(c) == std::string_view::npos ? c : '_');
        // excel also refuses a name that starts or ends with an apostrophe
        while (!base.empty() && base.front() == '\'')
            base.erase(0, 1);
        while (!base.empty() && base.back() == '\'')
            base.pop_back();
        if (base.empty())
            base = "Sheet";
        const auto cut = [](std::string s, size_t n) {
            if (s.size() <= n)
                return s;
            while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
                --n;
            return s.substr(0, n);
        };
        const auto isTaken = [&](const std::string& name) {
            return std::ranges::any_of(taken,
                                       [&](const std::string& t) { return lower(t) == lower(name); });
        };
        std::string name = cut(base, 31);
        for (int n = 2; isTaken(name); ++n) {
            const std::string suffix = std::format(" ({})", n);
            name = cut(base, 31 - suffix.size()) + suffix;
        }
        return name;
    }

    std::optional<double> plainNumber(const std::string& v) {
        size_t i = v.starts_with('-') ? 1 : 0;
        const size_t intStart = i;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])))
            ++i;
        const size_t intLen = i - intStart;
        if (intLen == 0 || (intLen > 1 && v[intStart] == '0'))
            return std::nullopt; // "", "-", "007"
        size_t fracLen = 0;
        if (i < v.size() && v[i] == '.') {
            const size_t fracStart = ++i;
            while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])))
                ++i;
            fracLen = i - fracStart;
            if (fracLen == 0)
                return std::nullopt; // "1."
        }
        if (i != v.size())
            return std::nullopt; // exponents, spaces, units: left as text
        // significant digits: what a double keeps exactly
        std::string sig = v.substr(intStart, intLen) + (fracLen ? v.substr(intStart + intLen + 1) : "");
        sig.erase(0, std::min(sig.find_first_not_of('0'), sig.size()));
        if (fracLen)
            while (!sig.empty() && sig.back() == '0')
                sig.pop_back();
        if (sig.size() > 15)
            return std::nullopt;
        // a c-locale stream: floating from_chars needs macos 26
        std::istringstream in(v);
        in.imbue(std::locale::classic());
        double d = 0;
        if (!(in >> d))
            return std::nullopt;
        return d;
    }

    std::optional<double> dateSerial(const std::string& v) {
        int y = 0, mo = 0, d = 0;
        if (v.size() < 10 || v[4] != '-' || v[7] != '-' || !digits(v, 0, 4, y) ||
            !digits(v, 5, 2, mo) || !digits(v, 8, 2, d) || mo < 1 || mo > 12 || d < 1 || d > 31)
            return std::nullopt;
        // excel's 1900 system starts on 1900-03-01 for exact serials (it believes 1900 was leap)
        if (y < 1900 || (y == 1900 && mo < 3) || y > 9999)
            return std::nullopt;
        double serial =
            static_cast<double>(daysFromCivil(y, mo, d) - daysFromCivil(1899, 12, 30));
        if (v.size() == 10)
            return serial;
        int h = 0, mi = 0, s = 0;
        if ((v[10] != ' ' && v[10] != 'T') || v.size() < 19 || v[13] != ':' || v[16] != ':' ||
            !digits(v, 11, 2, h) || !digits(v, 14, 2, mi) || !digits(v, 17, 2, s) || h > 23 ||
            mi > 59 || s > 60)
            return std::nullopt;
        double frac = 0;
        size_t i = 19;
        if (i < v.size() && v[i] == '.') {
            double scale = 0.1;
            for (++i; i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])); ++i, scale /= 10)
                frac += (v[i] - '0') * scale;
        }
        if (i != v.size())
            return std::nullopt; // an offset ("+02") or anything else: not ours to shift
        return serial + (h * 3600.0 + mi * 60.0 + s + frac) / 86400.0;
    }

    struct Writer::Impl {
        std::string path, tmp;
        size_t rowsPerSheet = 0;
        lxw_workbook* wb = nullptr;
        std::string error;
        std::vector<std::string> names; // sheets written, in order
        size_t truncated = 0;

        // current sheet
        std::string baseName;
        std::vector<std::string> columns, types;
        std::vector<size_t> widths;
        lxw_worksheet* sheet = nullptr;
        lxw_row_t nextRow = 0; // 0-based
        size_t rowsInSheet = 0;
        int part = 1;

        lxw_format* headerFmt = nullptr;
        lxw_format* dateFmt = nullptr;
        lxw_format* dateTimeFmt = nullptr;

        void check(lxw_error err) {
            if (err != LXW_NO_ERROR && error.empty())
                error = lxw_strerror(err);
        }

        void finishSheet() {
            if (!sheet)
                return;
            for (size_t c = 0; c < widths.size(); ++c)
                check(worksheet_set_column(sheet, static_cast<lxw_col_t>(c),
                                           static_cast<lxw_col_t>(c),
                                           static_cast<double>(std::clamp<size_t>(widths[c] + 2, 8, 60)),
                                           nullptr));
            if (!columns.empty())
                check(worksheet_autofilter(sheet, 0, 0, nextRow > 0 ? nextRow - 1 : 0,
                                           static_cast<lxw_col_t>(columns.size() - 1)));
            sheet = nullptr;
        }

        void openSheet(const std::string& name) {
            finishSheet();
            const std::string unique = sheetName(name, names);
            sheet = workbook_add_worksheet(wb, unique.c_str());
            if (!sheet)
                throw std::runtime_error(std::format("could not add the sheet '{}'", unique));
            names.push_back(unique);
            worksheet_freeze_panes(sheet, 1, 0);
            widths.assign(columns.size(), 0);
            for (size_t c = 0; c < columns.size(); ++c) {
                check(worksheet_write_string(sheet, 0, static_cast<lxw_col_t>(c),
                                             cleanText(columns[c], truncated).c_str(), headerFmt));
                widths[c] = columns[c].size();
            }
            nextRow = 1;
            rowsInSheet = 0;
        }

        template <typename F> void guard(F&& f) {
            if (!error.empty())
                return;
            try {
                f();
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
    };

    Writer::Writer(std::string path, size_t rowsPerSheet) : impl_(std::make_unique<Impl>()) {
        auto& m = *impl_;
        m.path = std::move(path);
        m.tmp = m.path + ".part";
        m.rowsPerSheet = std::max<size_t>(rowsPerSheet, 1);
        // rows stream through temp files, so memory stays flat however big the export
        lxw_workbook_options options{};
        options.constant_memory = LXW_TRUE;
        options.use_zip64 = LXW_TRUE;
        m.wb = workbook_new_opt(m.tmp.c_str(), &options);
        if (!m.wb) {
            m.error = std::format("could not create '{}'", m.tmp);
            return;
        }
        m.headerFmt = workbook_add_format(m.wb);
        format_set_bold(m.headerFmt);
        m.dateFmt = workbook_add_format(m.wb);
        format_set_num_format(m.dateFmt, "yyyy-mm-dd");
        m.dateTimeFmt = workbook_add_format(m.wb);
        format_set_num_format(m.dateTimeFmt, "yyyy-mm-dd hh:mm:ss");
    }

    Writer::~Writer() {
        if (impl_->wb) // save() never ran: close to free it, then drop the half file
            workbook_close(impl_->wb);
        std::error_code ec;
        std::filesystem::remove(impl_->tmp, ec);
    }

    void Writer::beginSheet(const std::string& name, const std::vector<std::string>& columns,
                            const std::vector<std::string>& columnTypes) {
        impl_->guard([&] {
            if (columns.size() > 16384)
                throw std::runtime_error(std::format(
                    "{} columns: an Excel sheet holds 16,384 at most", columns.size()));
            impl_->baseName = name;
            impl_->columns = columns;
            impl_->types = columnTypes;
            impl_->part = 1;
            impl_->openSheet(name);
        });
    }

    void Writer::addRow(const std::vector<std::string>& row) {
        impl_->guard([&] {
            auto& m = *impl_;
            if (!m.sheet)
                throw std::runtime_error("addRow before beginSheet");
            if (m.rowsInSheet >= m.rowsPerSheet) // past excel's limit: carry on in a new sheet
                m.openSheet(std::format("{} ({})", m.baseName, ++m.part));
            const size_t n = std::min(row.size(), m.columns.size());
            for (size_t c = 0; c < n; ++c) {
                const std::string& v = row[c];
                const auto col = static_cast<lxw_col_t>(c);
                const std::string& type = c < m.types.size() ? m.types[c] : std::string();
                if (isNullSentinel(v))
                    continue; // an empty cell
                if (isBoolSentinel(v)) {
                    m.check(worksheet_write_boolean(m.sheet, m.nextRow, col,
                                                    boolSentinelValue(v), nullptr));
                    continue;
                }
                if (c < m.widths.size() && m.rowsInSheet < 200)
                    m.widths[c] = std::max(m.widths[c], std::min<size_t>(v.size(), 60));
                if (!type.empty() && isDateType(type)) {
                    if (const auto serial = dateSerial(v)) {
                        m.check(worksheet_write_number(m.sheet, m.nextRow, col, *serial,
                                                       v.size() == 10 ? m.dateFmt : m.dateTimeFmt));
                        continue;
                    }
                } else if (type.empty() || !isTextType(type)) {
                    if (const auto num = plainNumber(v)) {
                        m.check(worksheet_write_number(m.sheet, m.nextRow, col, *num, nullptr));
                        continue;
                    }
                }
                m.check(worksheet_write_string(m.sheet, m.nextRow, col,
                                               cleanText(v, m.truncated).c_str(), nullptr));
            }
            ++m.nextRow;
            ++m.rowsInSheet;
        });
    }

    bool Writer::save(std::string& error) {
        auto& m = *impl_;
        m.guard([&] {
            m.finishSheet();
            const lxw_error err = workbook_close(m.wb);
            m.wb = nullptr;
            m.check(err);
            if (m.error.empty())
                std::filesystem::rename(m.tmp, m.path);
        });
        if (m.truncated > 0)
            spdlog::warn("xlsx: {} cell(s) cut at {} characters", m.truncated, kMaxCellChars);
        error = m.error;
        return error.empty();
    }

    size_t Writer::sheets() const {
        return impl_->names.size();
    }
    size_t Writer::truncatedCells() const {
        return impl_->truncated;
    }

} // namespace xlsx
