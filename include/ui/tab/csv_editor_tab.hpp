#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include "utils/csv_document.hpp"
#include <memory>
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

// edits a csv file in memory: cells, rows, sort, undo; Save writes the file in its
// own format. DuckDB only reads the saved file, for the WHERE filter
class CsvEditorTab final : public Tab {
public:
    [[nodiscard]] bool hasBottomBar() const override {
        return true;
    }
    CsvEditorTab(const std::string& name, std::string filePath);
    ~CsvEditorTab() override;

    void render() override;
    [[nodiscard]] bool hasUnsavedChanges() const override;

    [[nodiscard]] const std::string& getFilePath() const {
        return filePath_;
    }

private:
    enum class Dialog { None, ConfirmReload, Message };

    std::string filePath_;
    CsvDocument doc_;

    // load
    AsyncOperation<CsvLoadResult> loadOp_;
    std::string loadError_;

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

    Dialog dialog_ = Dialog::None;
    bool dialogOpening_ = false;
    std::string message_;
    std::string statusText_;

    void startLoad();
    void save();
    void markChanged();
    void rebuildTable();
    void initTable();

    // view <-> document rows
    [[nodiscard]] int docRow(int viewRow) const;
    [[nodiscard]] int viewRowCount() const;
    [[nodiscard]] std::vector<int> selectedDocRows() const;
    void deleteDocRows(const std::vector<int>& rows);

    void applyFilter();
    void clearFilter();
    void sortColumn(int col, bool ascending);
    void undoRedo(bool redo);
    void openDialog(Dialog dialog);
    void showMessage(std::string message);

    void handleShortcuts();
    void renderToolbar();
    void renderTableView();
    void renderStatusBar();
    void renderDialogs();
    void renderCellMenu(int row, int col);
};
