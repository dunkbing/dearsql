#pragma once

#include <cstddef>
#include <deque>
#include <optional>
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
    // reads and parses a file, detecting its dialect
    bool load(const std::string& path, std::string& error);
    // writes through a temp file and a rename, so a failed save leaves the file intact
    bool save(const std::string& path, std::string& error);

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
    void sortBy(int col, bool ascending);

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
    };
    struct UndoStep {
        // a cell edit keeps just the cell, anything else the whole table
        int row = -1;
        int col = -1;
        std::string value;
        std::optional<Snapshot> snapshot;
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
    void padRow(std::vector<std::string>& row) const;

    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;
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
