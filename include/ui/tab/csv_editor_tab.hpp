#pragma once

#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include "ui/text_editor.hpp"
#include <memory>
#include <string>
#include <vector>

class CsvEditorTab final : public Tab {
public:
    CsvEditorTab(const std::string& name, std::string filePath);

    void render() override;
    [[nodiscard]] bool hasUnsavedChanges() const override;

    [[nodiscard]] const std::string& getFilePath() const {
        return filePath_;
    }

private:
    enum class ViewMode { Table, Raw };

    std::string filePath_;
    // parsed data
    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> rows_;

    // raw text editor (for raw view)
    dearsql::TextEditor rawEditor_;
    bool rawPaletteSet_ = false;
    bool rawLastDark_ = true;

    ViewMode viewMode_ = ViewMode::Table;

    // edit state
    bool hasChanges_ = false;
    bool loadError_ = false;
    std::string errorMessage_;
    bool hasValidationError_ = false;
    std::string validationError_;

    // table renderer (table view)
    std::unique_ptr<TableRenderer> tableRenderer_;
    bool tableRendererDataDirty_ = false;
    int selectedRow_ = -1;
    int selectedCol_ = -1;

    // raw view dirty flag (raw buffer was edited)
    bool rawDirty_ = false;

    void loadFile();
    void saveFile();

    void syncTableToRaw();
    bool syncRawToTable();

    void initTableRenderer();
    void syncTableRendererData();
    void clearValidationError();
    void setValidationError(std::string message);

    [[nodiscard]] bool hasPendingChanges() const;
    void renderToolbar();
    void renderTableView();
    void renderRawView();
};
