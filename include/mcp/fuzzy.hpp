#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

// typo-tolerant name matching for schema search: case, '_', '-' and '.' are
// ignored, so "userId", "user_id" and "USER-ID" are the same name
namespace fuzzy {

    inline std::string normalize(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c != '_' && c != '-' && c != ' ' && c != '.')
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    inline size_t editDistance(std::string_view a, std::string_view b) {
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j)
            row[j] = j;
        for (size_t i = 1; i <= a.size(); ++i) {
            size_t diag = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); ++j) {
                const size_t up = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] != b[j - 1])});
                diag = up;
            }
        }
        return row[b.size()];
    }

    // 0 = no match; exact > prefix > substring > in-order letters > small typo
    inline int score(std::string_view query, std::string_view candidate) {
        const std::string q = normalize(query);
        const std::string c = normalize(candidate);
        if (q.empty() || c.empty())
            return 0;
        if (q == c)
            return 1000;
        if (c.starts_with(q))
            return 800 - static_cast<int>(std::min<size_t>(c.size() - q.size(), 100));
        if (auto pos = c.find(q); pos != std::string::npos)
            return 600 - static_cast<int>(std::min<size_t>(pos, 100));
        // letters in order with few gaps ("usrrl" -> "user_roles")
        size_t ci = 0, gaps = 0, matched = 0;
        for (char qc : q) {
            const size_t found = c.find(qc, ci);
            if (found == std::string::npos)
                break;
            gaps += found - ci;
            ci = found + 1;
            ++matched;
        }
        if (matched == q.size() && q.size() >= 3 && gaps <= q.size() * 2)
            return 400 - static_cast<int>(std::min<size_t>(gaps * 10, 200));
        // typo: one edit for short names, two from five letters (a swap is two), a
        // quarter of the name for long ones
        const size_t dist = editDistance(q, c);
        const size_t allowed = q.size() < 5 ? 1 : std::max<size_t>(2, q.size() / 4);
        if (dist <= allowed && dist < q.size())
            return 200 - static_cast<int>(dist * 40);
        return 0;
    }

} // namespace fuzzy
