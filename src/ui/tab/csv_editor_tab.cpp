#include "ui/tab/csv_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "dearsql/backends/duckdb_connection.hpp"
#include "dearsql/types.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/auto_complete_input.hpp"
#include "utils/button.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <spdlog/spdlog.h>

namespace {
#ifdef __APPLE__
    constexpr const char* kMod = "Cmd";
#else
    constexpr const char* kMod = "Ctrl";
#endif

    std::string shortcut(const char* key, bool shift = false) {
        return std::format("{}+{}{}", kMod, shift ? "Shift+" : "", key);
    }

    std::string quoteIdent(const std::string& name) {
        std::string out = "\"";
        for (char c : name)
            out += c == '"' ? std::string("\"\"") : std::string(1, c);
        return out + "\"";
    }

    std::string quoteLiteral(const std::string& value) {
        std::string out = "'";
        for (char c : value)
            out += c == '\'' ? std::string("''") : std::string(1, c);
        return out + "'";
    }

    std::string trim(std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    void copyTo(char* buffer, std::size_t size, const std::string& value) {
        std::strncpy(buffer, value.c_str(), size - 1);
        buffer[size - 1] = '\0';
    }

    const char* lineBreakName(const std::string& lb) {
        return lb == "\r\n" ? "CRLF" : lb == "\r" ? "CR" : "LF";
    }
} // namespace

CsvEditorTab::CsvEditorTab(const std::string& name, std::string filePath)
    : Tab(name, TabType::CSV_EDITOR), filePath_(std::move(filePath)) {
    AutoCompleteInput::Config config;
    config.hint = "Filter rows (SQL WHERE), e.g. age > 30";
    config.width = 380.0f;
    config.onSubmit = [this] { applyFilter(); };
    config.endIcon = ICON_FA_MAGNIFYING_GLASS;
    config.onEndIconClick = [this] { applyFilter(); };
    config.keywords = {"AND", "OR",   "NOT",  "IN",    "LIKE", "ILIKE", "BETWEEN",
                       "IS",  "NULL", "TRUE", "FALSE", "CAST", "LOWER", "UPPER"};
    filterInput_ = std::make_unique<AutoCompleteInput>(config);

    initTable();
    startLoad();
}

CsvEditorTab::~CsvEditorTab() = default;

bool CsvEditorTab::hasUnsavedChanges() const {
    return doc_.isModified() || (table_ && table_->isEditing());
}

void CsvEditorTab::startLoad() {
    if (loadOp_.isRunning())
        return;
    loadError_.clear();
    if (table_ && table_->isEditing())
        table_->exitEditMode(false);
    loadOp_.start([path = filePath_]() {
        CsvLoadResult result;
        result.ok = result.doc.load(path, result.error);
        return result;
    });
}

void CsvEditorTab::markChanged() {
    tableDirty_ = true;
    statusText_.clear();
}

void CsvEditorTab::save() {
    if (table_->isEditing())
        table_->exitEditMode(true);
    std::string error;
    if (!doc_.save(filePath_, error)) {
        spdlog::error("Failed to save CSV {}: {}", filePath_, error);
        showMessage("Could not save the file.\n\n" + error);
        return;
    }
    statusText_ = "Saved";
}

// ---------------------------------------------------------------- table

void CsvEditorTab::initTable() {
    TableRenderer::Config cfg;
    cfg.allowEditing = true;
    cfg.allowSelection = true;
    cfg.showRowNumbers = true;
    cfg.clearOnDelete = true;
    cfg.minHeight = 200.0f;
    table_ = std::make_unique<TableRenderer>(cfg);

    // ponytail: the grid holds its own copy of the rows, rebuilt after structural edits;
    // a renderer reading the document directly if files outgrow 2x memory
    table_->setOnCellEdit([this](int row, int col, const std::string& value) {
        doc_.setCell(docRow(row), col, value);
        table_->setCellValue(row, col, value);
        statusText_.clear();
    });
    table_->setOnRangeEdit(
        [this](int row, int col, const std::vector<std::vector<std::string>>& values) {
            if (!filtered_) {
                doc_.setRange(row, col, values);
            } else {
                // a filtered view writes into the rows it shows, without growing
                std::vector<CsvDocument::CellValue> cells;
                for (std::size_t r = 0; r < values.size(); ++r) {
                    const int vr = row + static_cast<int>(r);
                    if (vr >= viewRowCount())
                        break;
                    for (std::size_t c = 0; c < values[r].size(); ++c)
                        cells.push_back({docRow(vr), col + static_cast<int>(c), values[r][c]});
                }
                doc_.setCells(cells);
            }
            markChanged();
        });
    table_->setOnDeleteRow([this](int row) {
        auto rows = selectedDocRows();
        if (rows.empty())
            rows.push_back(docRow(row));
        deleteDocRows(rows);
    });
    table_->setOnFilterByValue([this](int, int col, const std::string& value) {
        if (col < 0 || col >= doc_.columnCount())
            return;
        const std::string current = trim(filterBuffer_);
        const std::string clause =
            std::format("{} = {}", quoteIdent(doc_.headers()[col]), quoteLiteral(value));
        copyTo(filterBuffer_, sizeof(filterBuffer_),
               current.empty() ? clause : std::format("({}) AND {}", current, clause));
        applyFilter();
    });
    table_->setOnSortChanged([this](int col, const std::string&, SortDirection direction) {
        // sorting reorders the rows themselves, so the header shows no sort state
        table_->setSortColumn(-1, SortDirection::None);
        if (col >= 0 && direction != SortDirection::None)
            sortColumn(col, direction == SortDirection::Ascending);
    });
    table_->setRowNumberCallback([this](int row) { return docRow(row) + 1; });
    table_->setCellMenuExtension([this](int row, int col) { renderCellMenu(row, col); });
}

void CsvEditorTab::rebuildTable() {
    table_->setColumns(doc_.headers());
    if (!filtered_) {
        table_->setData(doc_.rows());
    } else {
        std::vector<std::vector<std::string>> rows;
        rows.reserve(viewRows_.size());
        for (int r : viewRows_)
            rows.push_back(doc_.rows()[r]);
        table_->setData(std::move(rows));
    }
    // keep the selection inside the table
    int r0, c0, r1, c1;
    if (table_->getSelectionRange(r0, c0, r1, c1) &&
        (r0 >= viewRowCount() || c0 >= doc_.columnCount()))
        table_->setSelectedCell(-1, -1);
    tableDirty_ = false;
}

int CsvEditorTab::docRow(int viewRow) const {
    if (!filtered_)
        return viewRow;
    return viewRow >= 0 && viewRow < static_cast<int>(viewRows_.size()) ? viewRows_[viewRow] : -1;
}

int CsvEditorTab::viewRowCount() const {
    return filtered_ ? static_cast<int>(viewRows_.size()) : doc_.rowCount();
}

std::vector<int> CsvEditorTab::selectedDocRows() const {
    std::vector<int> rows;
    for (int r : table_->getSelectedRows())
        if (const int d = docRow(r); d >= 0)
            rows.push_back(d);
    return rows;
}

void CsvEditorTab::deleteDocRows(const std::vector<int>& rows) {
    if (rows.empty())
        return;
    doc_.deleteRows(rows);
    if (filtered_) {
        // drop the deleted rows from the view and shift the ones after them
        std::vector<int> sorted = rows;
        std::ranges::sort(sorted);
        std::vector<int> kept;
        for (int r : viewRows_) {
            if (std::ranges::binary_search(sorted, r))
                continue;
            kept.push_back(r -
                           static_cast<int>(std::ranges::lower_bound(sorted, r) - sorted.begin()));
        }
        viewRows_ = std::move(kept);
    }
    table_->setSelectedCell(-1, -1);
    markChanged();
}

// ---------------------------------------------------------------- operations

void CsvEditorTab::applyFilter() {
    const std::string where = trim(filterBuffer_);
    if (where.empty()) {
        clearFilter();
        return;
    }
    if (filterOp_.isRunning())
        return;
    if (doc_.isModified()) {
        filterError_ = "Save your changes first: the filter reads the file as saved.";
        return;
    }
    filterError_.clear();
    const CsvDialect& d = doc_.dialect();
    dearsql::CsvFormat format;
    format.delimiter = d.delimiter;
    format.quote = d.quote;
    format.header = d.hasHeader;
    format.encoding = d.encoding == CsvDialect::Encoding::Utf8          ? "utf-8"
                      : d.encoding == CsvDialect::Encoding::Windows1252 ? "latin-1"
                                                                        : "utf-16";
    const int total = doc_.rowCount();
    filterOp_.start([path = filePath_, where, format, total]() {
        CsvFilterResult result;
        try {
            for (std::size_t r : dearsql::csvMatchingRows(path, where, format)) {
                if (static_cast<int>(r) >= total) {
                    result.error = "DuckDB read more rows than the editor did, so the "
                                   "filter can't be matched to the rows here.";
                    return result;
                }
                result.rows.push_back(static_cast<int>(r));
            }
            result.ok = true;
        } catch (const std::exception& e) {
            result.error = e.what();
        }
        return result;
    });
    activeFilter_ = where;
}

void CsvEditorTab::clearFilter() {
    filterBuffer_[0] = '\0';
    activeFilter_.clear();
    filterError_.clear();
    if (filtered_) {
        filtered_ = false;
        viewRows_.clear();
        table_->setSelectedCell(-1, -1);
        markChanged();
    }
}

void CsvEditorTab::undoRedo(bool redo) {
    const bool structural = redo ? doc_.redoIsStructural() : doc_.undoIsStructural();
    if (!(redo ? doc_.redo() : doc_.undo()))
        return;
    // the filter's rows index the table as it was: a structural step invalidates them
    if (filtered_ && structural)
        clearFilter();
    markChanged();
}

void CsvEditorTab::sortColumn(int col, bool ascending) {
    if (filtered_) {
        showMessage("Clear the filter to sort: sorting reorders the whole file.");
        return;
    }
    doc_.sortBy(col, ascending);
    markChanged();
}

void CsvEditorTab::openDialog(Dialog dialog) {
    dialog_ = dialog;
    dialogOpening_ = true;
}

void CsvEditorTab::showMessage(std::string message) {
    message_ = std::move(message);
    openDialog(Dialog::Message);
}

// ---------------------------------------------------------------- render

void CsvEditorTab::render() {
    const auto& colors = Application::getInstance().getCurrentColors();

    loadOp_.check([this](CsvLoadResult r) {
        if (!r.ok) {
            loadError_ = r.error;
            spdlog::error("CSV load failed: {}", r.error);
            return;
        }
        doc_ = std::move(r.doc);
        filtered_ = false;
        viewRows_.clear();
        activeFilter_.clear();
        table_->setSelectedCell(-1, -1);
        filterInput_->setKeywords(
            {"AND", "OR", "NOT", "IN", "LIKE", "ILIKE", "BETWEEN", "IS", "NULL", "TRUE", "FALSE"});
        filterInput_->addKeywords(doc_.headers());
        markChanged();
        statusText_.clear();
    });
    filterOp_.check([this](CsvFilterResult r) {
        if (!r.ok) {
            filterError_ = r.error;
            activeFilter_.clear();
            return;
        }
        viewRows_ = std::move(r.rows);
        filtered_ = true;
        table_->setSelectedCell(-1, -1);
        markChanged();
    });

    if (loadOp_.isRunning()) {
        ImGui::Spacing();
        UIUtils::Spinner("##csv_loading", 8.0f, 2, ImGui::GetColorU32(colors.peach));
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(colors.subtext0, "Loading...");
        return;
    }
    if (!loadError_.empty()) {
        ImGui::TextColored(colors.red, "%s", loadError_.c_str());
        if (UIUtils::Button(ICON_FA_ARROWS_ROTATE " Retry"))
            startLoad();
        return;
    }

    handleShortcuts();

    renderToolbar();
    // same gap under the toolbar as the table viewer
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));

    if (!filterError_.empty())
        ImGui::TextColored(colors.red, "%s", filterError_.c_str());

    // the grid takes what is left above the status bar
    const float statusH = ImGui::GetTextLineHeightWithSpacing() + Theme::Spacing::XS;
    const float gridH = std::max(ImGui::GetContentRegionAvail().y - statusH, 100.0f);
    if (ImGui::BeginChild("##csv_body", ImVec2(0, gridH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        renderTableView();
    }
    ImGui::EndChild();
    renderStatusBar();
    renderDialogs();
}

void CsvEditorTab::handleShortcuts() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    const ImGuiIO& io = ImGui::GetIO();
    if (!(io.KeyMods & ImGuiMod_Shortcut))
        return;
    const bool editingText = io.WantTextInput || table_->isEditing();

    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        save();
    } else if (!editingText && ((ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyShift) ||
                                ImGui::IsKeyPressed(ImGuiKey_Y, false))) {
        undoRedo(true);
    } else if (!editingText && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        undoRedo(false);
    }
}

void CsvEditorTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const bool modified = hasUnsavedChanges();

    // save / revert
    if (!modified)
        ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
    if (UIUtils::IconButton(ICON_FA_FLOPPY_DISK, UIUtils::ButtonVariant::Primary))
        save();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Save (%s)", shortcut("S").c_str());
    ImGui::SameLine(0, Theme::Spacing::XS);
    ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
    if (UIUtils::IconButton(ICON_FA_XMARK, UIUtils::ButtonVariant::Danger))
        openDialog(Dialog::ConfirmReload);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Revert to the saved file");
    if (!modified)
        ImGui::EndDisabled();

    // undo / redo
    ImGui::SameLine(0, Theme::Spacing::M);
    ImGui::BeginDisabled(!doc_.canUndo());
    if (UIUtils::IconButton(ICON_FA_ROTATE_LEFT))
        undoRedo(false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo (%s)", shortcut("Z").c_str());
    ImGui::SameLine(0, Theme::Spacing::XS);
    ImGui::BeginDisabled(!doc_.canRedo());
    if (UIUtils::IconButton(ICON_FA_ROTATE_RIGHT))
        undoRedo(true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo (%s)", shortcut("Z", true).c_str());

    // filter
    ImGui::SameLine(0, Theme::Spacing::L);
    filterInput_->render("##csv_filter", filterBuffer_, sizeof(filterBuffer_));
    if (filterOp_.isRunning()) {
        ImGui::SameLine(0, Theme::Spacing::S);
        UIUtils::Spinner("##csv_filtering", 6.0f, 2, ImGui::GetColorU32(colors.blue));
    } else if (filtered_ || !activeFilter_.empty()) {
        ImGui::SameLine(0, Theme::Spacing::XS);
        if (UIUtils::IconButton(ICON_FA_FILTER_CIRCLE_XMARK))
            clearFilter();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear filter");
    }

    if (modified) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(colors.peach, "Unsaved changes");
    }
}

void CsvEditorTab::renderCellMenu(int row, int) {
    // inserting rows into a filtered view would land them out of sight
    if (ImGui::MenuItem("Insert Row Above", nullptr, false, !filtered_)) {
        doc_.insertRows(docRow(row));
        markChanged();
    }
    if (ImGui::MenuItem("Insert Row Below", nullptr, false, !filtered_)) {
        doc_.insertRows(docRow(row) + 1);
        markChanged();
    }
}

void CsvEditorTab::renderTableView() {
    if (tableDirty_)
        rebuildTable();
    if (doc_.columnCount() == 0) {
        ImGui::TextDisabled("This file is empty.");
        return;
    }
    if (viewRowCount() == 0) {
        ImGui::TextDisabled(filtered_ ? "No rows match the filter." : "No rows.");
        if (!filtered_ && UIUtils::Button(ICON_FA_PLUS " Add Row")) {
            doc_.insertRows(0);
            markChanged();
        }
        return;
    }
    table_->render("##csv_table");
}

void CsvEditorTab::renderStatusBar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const CsvDialect& d = doc_.dialect();
    std::string text = filtered_ ? std::format("{} of {} rows", viewRowCount(), doc_.rowCount())
                                 : std::format("{} rows", doc_.rowCount());
    text += std::format(" · {} columns · {} · {}{} · {}", doc_.columnCount(),
                        csv::delimiterName(d.delimiter), csv::encodingName(d.encoding),
                        d.bom ? " BOM" : "", lineBreakName(d.lineBreak));
    if (!d.hasHeader)
        text += " · no header row";
    ImGui::TextColored(colors.subtext0, "%s", text.c_str());
    if (doc_.raggedRows() > 0) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(colors.peach, ICON_FA_TRIANGLE_EXCLAMATION " %d rows were padded",
                           doc_.raggedRows());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("These rows had fewer fields than the widest row; saving writes "
                              "them with empty fields.");
    }
    if (!statusText_.empty()) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(colors.green, "%s", statusText_.c_str());
    }
}

// ---------------------------------------------------------------- dialogs

void CsvEditorTab::renderDialogs() {
    if (dialog_ == Dialog::None)
        return;
    const char* id = "##csv_dialog";
    if (dialogOpening_) {
        ImGui::OpenPopup(id);
        dialogOpening_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::L));
    if (!ImGui::BeginPopupModal(id, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PopStyleVar();
        dialog_ = Dialog::None;
        return;
    }
    bool close = false;
    if (dialog_ == Dialog::Message) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(message_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (UIUtils::Button("OK", UIUtils::ButtonVariant::Primary) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
    } else if (dialog_ == Dialog::ConfirmReload) {
        ImGui::TextUnformatted("Discard unsaved changes and reload the file?");
        ImGui::Spacing();
        ImGui::Spacing();
        if (UIUtils::Button("Reload", UIUtils::ButtonVariant::Primary) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            startLoad();
            close = true;
        }
        ImGui::SameLine(0, Theme::Spacing::S);
        if (UIUtils::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
    } else {
        close = true;
    }

    if (close) {
        ImGui::CloseCurrentPopup();
        message_.clear();
        dialog_ = Dialog::None;
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}
