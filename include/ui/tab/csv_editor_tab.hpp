#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include "utils/csv_document.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AutoCompleteInput;

struct CsvLoadResult {
    bool ok = false;
    std::string error;
    CsvDocument doc;
};

struct CsvFilterResult {
    bool ok = false;
    std::string error;
    std::vector<int> rows;
};

// edits a csv file in memory, Tablecruncher-style: rows, columns, find/replace, sort,
// flags, column tools; Save writes the file in its own format. DuckDB only reads the
// saved file, for the WHERE filter
class CsvEditorTab final : public Tab {
public:
    CsvEditorTab(const std::string& name, std::string filePath);
    ~CsvEditorTab() override;

    void render() override;
    [[nodiscard]] bool hasUnsavedChanges() const override;

    [[nodiscard]] const std::string& getFilePath() const {
        return filePath_;
    }

private:
    enum class Dialog {
        None,
        Properties,
        SplitColumn,
        MergeColumns,
        SplitFile,
        JumpTo,
        RenameColumn,
        ConfirmReload,
        Message
    };

    std::string filePath_;
    CsvDocument doc_;

    // load
    AsyncOperation<CsvLoadResult> loadOp_;
    std::string loadError_;
    std::optional<CsvDialect> reloadDialect_; // "open with format"

    // table view; viewRows_ maps view rows to document rows while a filter is on
    std::unique_ptr<TableRenderer> table_;
    bool tableDirty_ = true;
    std::vector<int> viewRows_;
    bool filtered_ = false;

    // filter (DuckDB WHERE over the saved file)
    std::unique_ptr<AutoCompleteInput> filterInput_;
    char filterBuffer_[2048] = {};
    std::string activeFilter_;
    std::string filterError_;
    AsyncOperation<CsvFilterResult> filterOp_;

    // find & replace
    bool findOpen_ = false;
    bool focusFind_ = false;
    char findBuffer_[512] = {};
    char replaceBuffer_[512] = {};
    CsvDocument::FindOptions find_;
    std::optional<CsvDocument::Matcher> matcher_;
    bool onlyCurrentColumn_ = false;
    std::string findStatus_;

    // dialogs
    Dialog dialog_ = Dialog::None;
    bool dialogOpening_ = false;
    std::string message_;
    CsvDialect editDialect_;
    char inputBuffer_[256] = {};
    int inputNumber_ = 0;
    int inputNumber2_ = 0;
    int dialogColumn_ = -1;
    std::string statusText_;

    void startLoad();
    void save();
    void saveAs();
    void markChanged();
    void rebuildTable();
    void initTable();

    // view <-> document rows
    [[nodiscard]] int docRow(int viewRow) const;
    [[nodiscard]] int viewRowOf(int docRow) const;
    [[nodiscard]] int viewRowCount() const;
    [[nodiscard]] std::vector<int> selectedDocRows() const;
    // selected columns, or the selected cell's column
    [[nodiscard]] bool selectedColumns(int& first, int& last) const;
    void selectCell(int docRow, int col);
    void deleteDocRows(const std::vector<int>& rows);

    void applyFilter();
    void clearFilter();
    void findNext(bool forward);
    void replaceCurrent();
    void replaceAll();
    void sortColumn(int col, bool ascending);
    void jumpToFlag(bool forward);

    void exportJson();
    void exportFlagged();
    void openDialog(Dialog dialog);
    void showMessage(std::string message);

    void handleShortcuts();
    void renderToolbar();
    void renderFileMenu();
    void renderEditMenu();
    void renderDataMenu();
    void renderFindBar();
    void renderTableView();
    void renderStatusBar();
    void renderDialogs();
    void renderColumnMenu(int col);
    void renderCellMenu(int row, int col);
};
