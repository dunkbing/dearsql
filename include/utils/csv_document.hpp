#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

// how a csv file is written, detected on load and reused on save so a file
// round-trips unchanged apart from the edits
struct CsvDialect {
    enum class Encoding { Utf8, Utf16LE, Utf16BE, Windows1252 };
    enum class QuoteStyle { Minimal, All, NonNumeric };

    std::string delimiter = ",";
    char quote = '"';
    std::string lineBreak = "\n";
    Encoding encoding = Encoding::Utf8;
    bool bom = false;
    bool hasHeader = true;
    QuoteStyle quoteStyle = QuoteStyle::Minimal;
    bool trailingLineBreak = true;

    bool operator==(const CsvDialect&) const = default;
};

namespace csv {
    // file bytes -> utf-8; sets the dialect's encoding and bom
    std::string decode(const std::string& bytes, CsvDialect& dialect);
    std::string encode(const std::string& utf8, const CsvDialect& dialect);

    // delimiter, line break and quote style of utf-8 text
    CsvDialect sniff(const std::string& text);

    std::vector<std::vector<std::string>> parse(const std::string& text, const CsvDialect& dialect);
    std::string serialize(const std::vector<std::vector<std::string>>& records,
                          const CsvDialect& dialect);
    void appendRecord(std::string& out, const std::vector<std::string>& record,
                      const CsvDialect& dialect);

    const char* encodingName(CsvDialect::Encoding encoding);
    std::string delimiterName(const std::string& delimiter);
} // namespace csv

// an editable csv file: rows in memory, edits undoable, saved in the file's own dialect
class CsvDocument {
public:
    struct FindOptions {
        std::string text;
        bool matchCase = false;
        bool wholeCell = false;
        bool regex = false;
        int column = -1; // -1 = every column
    };

    // FindOptions compiled once; an invalid regex matches nothing
    class Matcher {
    public:
        explicit Matcher(const FindOptions& find);
        bool operator()(const std::string& value) const;
        [[nodiscard]] bool valid() const {
            return valid_;
        }
        // value with every match replaced
        [[nodiscard]] std::string replace(const std::string& value,
                                          const std::string& replacement) const;

    private:
        FindOptions find_;
        std::string needle_; // lowercased unless matchCase
        std::optional<std::regex> regex_;
        bool valid_ = true;
    };

    // reads and parses a file; `dialect` overrides detection (open with format)
    bool load(const std::string& path, std::string& error,
              const std::optional<CsvDialect>& dialect = std::nullopt);
    // writes through a temp file and a rename, so a failed save leaves the file intact
    bool save(const std::string& path, std::string& error);
    // writes only the given rows (export flagged, split) without touching the saved state
    bool saveRows(const std::string& path, const std::vector<int>& rows, std::string& error) const;

    // the file as text, in its dialect (utf-8)
    [[nodiscard]] std::string toText() const;

    [[nodiscard]] const std::vector<std::string>& headers() const {
        return headers_;
    }
    [[nodiscard]] const std::vector<std::vector<std::string>>& rows() const {
        return rows_;
    }
    [[nodiscard]] int rowCount() const {
        return static_cast<int>(rows_.size());
    }
    [[nodiscard]] int columnCount() const {
        return static_cast<int>(headers_.size());
    }
    [[nodiscard]] const CsvDialect& dialect() const {
        return dialect_;
    }
    // format used from the next save on, as an undo step; hasHeader is switched with
    // setHasHeader
    void setDialect(const CsvDialect& dialect);
    // records whose field count differed from the widest record when loaded
    [[nodiscard]] int raggedRows() const {
        return raggedRows_;
    }

    // edits; each is one undo step
    void setCell(int row, int col, const std::string& value);
    // paste / clear a block at (row, col); grows the table when it runs past the end
    void setRange(int row, int col, const std::vector<std::vector<std::string>>& values);
    struct CellValue {
        int row;
        int col;
        std::string value;
    };
    // scattered cells (a paste into a filtered view); cells outside the table are skipped
    void setCells(const std::vector<CellValue>& cells);
    void insertRows(int at, int count = 1);
    void deleteRows(std::vector<int> rows);
    void insertColumn(int at, const std::string& name = {});
    void deleteColumns(int first, int last);
    // moves columns [first, last] one place left (-1) or right (+1)
    bool moveColumns(int first, int last, int delta);
    void renameColumn(int col, const std::string& name);
    void sortBy(int col, bool ascending);
    int replaceAll(const FindOptions& find, const std::string& replacement);
    // splits a column on a separator into as many columns as the longest split
    int splitColumn(int col, const std::string& separator);
    void mergeColumns(int first, int last, const std::string& separator);
    // first row becomes the header, or the header becomes the first row
    void setHasHeader(bool hasHeader);

    // flags: a row marker that is not saved to the file
    [[nodiscard]] bool isFlagged(int row) const {
        return row >= 0 && row < static_cast<int>(flags_.size()) && flags_[row];
    }
    void setFlags(const std::vector<int>& rows, bool flagged);
    void invertFlags();
    void clearFlags();
    [[nodiscard]] std::vector<int> flaggedRows() const;
    [[nodiscard]] int flaggedCount() const;
    // flags the rows whose value in `col` is not of the column's most common kind
    int flagInconsistent(int col);

    // next match after (row, col) in reading order, wrapping; rows limits the search
    // to a filtered subset (empty = all rows)
    [[nodiscard]] std::optional<std::pair<int, int>>
    findNext(const FindOptions& find, int row, int col, bool forward,
             const std::vector<int>& rows = {}) const;

    [[nodiscard]] std::string toJson(const std::vector<int>& rows = {}) const;
    // writes rowsPerFile-row files named <stem>_<n>.csv next to `path`
    std::vector<std::string> splitFile(const std::string& path, int rowsPerFile,
                                       std::string& error) const;

    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const {
        return !undo_.empty();
    }
    [[nodiscard]] bool canRedo() const {
        return !redo_.empty();
    }
    [[nodiscard]] bool isModified() const {
        return state_ != savedState_;
    }

private:
    struct Snapshot {
        std::vector<std::string> headers;
        std::vector<std::vector<std::string>> rows;
        std::vector<bool> flags;
        CsvDialect dialect;
    };
    struct UndoStep {
        // a cell edit keeps just the cell, a format change the dialect, anything else
        // the whole table
        int row = -1;
        int col = -1;
        std::string value;
        std::optional<Snapshot> snapshot;
        std::optional<CsvDialect> dialect;
        long state = 0; // document state this step restores
    };

    Snapshot snapshot() const;
    void restore(Snapshot s);
    // records the table before a structural edit
    void pushSnapshot();
    void pushStep(UndoStep step);
    // restores a step, returning its inverse
    UndoStep apply(UndoStep step);
    void applyRecords(std::vector<std::vector<std::string>> records);
    [[nodiscard]] std::vector<std::vector<std::string>> records(const std::vector<int>& rows,
                                                                bool all) const;
    void padRow(std::vector<std::string>& row) const;

    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;
    std::vector<bool> flags_;
    CsvDialect dialect_;
    int raggedRows_ = 0;

    // ponytail: structural undo keeps whole-table snapshots, capped at kMaxUndo;
    // diff-based steps if huge files need deep undo
    static constexpr std::size_t kMaxUndo = 30;
    std::deque<UndoStep> undo_;
    std::deque<UndoStep> redo_;
    long state_ = 0;
    long savedState_ = 0;
    long nextState_ = 0;
};
