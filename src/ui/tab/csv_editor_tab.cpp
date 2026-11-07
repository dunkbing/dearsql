#include "ui/tab/csv_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "dearsql/backends/duckdb_connection.hpp"
#include "dearsql/types.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/auto_complete_input.hpp"
#include "utils/button.hpp"
#include "utils/file_dialog.hpp"
#include "utils/menu.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
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

    const char* kDelimiterLabels[] = {"Comma (,)", "Semicolon (;)", "Tab",
                                      "Pipe (|)",  "Colon (:)",     "Other"};
    const char* kDelimiters[] = {",", ";", "\t", "|", ":"};
    const char* kLineBreakLabels[] = {"LF (Unix, macOS)", "CRLF (Windows)", "CR (classic Mac)"};
    const char* kLineBreaks[] = {"\n", "\r\n", "\r"};
    const char* kEncodingLabels[] = {"UTF-8", "UTF-16 LE", "UTF-16 BE", "Windows-1252"};
    const char* kQuoteStyleLabels[] = {"When needed", "Every field", "Non-numeric fields"};

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
    loadOp_.start([path = filePath_, dialect = reloadDialect_]() {
        CsvLoadResult result;
        result.ok = result.doc.load(path, result.error, dialect);
        return result;
    });
    reloadDialect_.reset();
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

void CsvEditorTab::saveAs() {
    const auto current = std::filesystem::path(filePath_);
    const std::string path =
        FileDialog::saveFile(current.filename().string(), "CSV Files", "csv,tsv,txt");
    if (path.empty())
        return;
    if (table_->isEditing())
        table_->exitEditMode(true);
    std::string error;
    if (!doc_.save(path, error)) {
        showMessage("Could not save the file.\n\n" + error);
        return;
    }
    filePath_ = path;
    setName(std::filesystem::path(path).filename().string());
    // the filter reads the file it was run against
    clearFilter();
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
    table_->setRowBgCallback([this](int row) -> unsigned int {
        if (!doc_.isFlagged(docRow(row)))
            return 0;
        const auto& c = Application::getInstance().getCurrentColors();
        return ImGui::GetColorU32(ImVec4(c.peach.x, c.peach.y, c.peach.z, 0.22f));
    });
    table_->setCellBgCallback([this](int row, int col) -> unsigned int {
        if (!findOpen_ || !matcher_)
            return 0;
        const int r = docRow(row);
        if (r < 0 || r >= doc_.rowCount() || !(*matcher_)(doc_.rows()[r][col]))
            return 0;
        const auto& c = Application::getInstance().getCurrentColors();
        return ImGui::GetColorU32(ImVec4(c.yellow.x, c.yellow.y, c.yellow.z, 0.28f));
    });
    table_->setColumnMenuExtension([this](int col) { renderColumnMenu(col); });
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

int CsvEditorTab::viewRowOf(int row) const {
    if (!filtered_)
        return row >= 0 && row < doc_.rowCount() ? row : -1;
    const auto it = std::ranges::lower_bound(viewRows_, row);
    return it != viewRows_.end() && *it == row ? static_cast<int>(it - viewRows_.begin()) : -1;
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

bool CsvEditorTab::selectedColumns(int& first, int& last) const {
    int r0, r1;
    return table_->getSelectionRange(r0, first, r1, last);
}

void CsvEditorTab::selectCell(int row, int col) {
    const int vr = viewRowOf(row);
    if (vr < 0)
        return;
    if (tableDirty_)
        rebuildTable();
    table_->setSelectedCell(vr, col);
    table_->scrollToCell(vr, col);
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
                    result.error = "DuckDB read more rows than the editor did; check the "
                                   "file's format in CSV Properties.";
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

void CsvEditorTab::findNext(bool forward) {
    find_.text = findBuffer_;
    if (find_.text.empty() || viewRowCount() == 0)
        return;
    // search from the selected cell; with nothing selected, from just before the first
    // cell (or after the last one going back)
    int r0, c0, r1, c1;
    int row, col;
    if (table_->getSelectionRange(r0, c0, r1, c1)) {
        row = docRow(r0);
        col = c0;
    } else {
        row = docRow(forward ? viewRowCount() - 1 : 0);
        col = forward ? doc_.columnCount() - 1 : 0;
    }
    find_.column = onlyCurrentColumn_ ? col : -1;
    const auto hit =
        doc_.findNext(find_, row, col, forward, filtered_ ? viewRows_ : std::vector<int>{});
    if (!hit) {
        findStatus_ = matcher_ && !matcher_->valid() ? "Invalid regular expression" : "No matches";
        return;
    }
    findStatus_.clear();
    selectCell(hit->first, hit->second);
}

void CsvEditorTab::replaceCurrent() {
    int r0, c0, r1, c1;
    if (!matcher_ || !table_->getSelectionRange(r0, c0, r1, c1)) {
        findNext(true);
        return;
    }
    const int row = docRow(r0);
    if (row >= 0 && (*matcher_)(doc_.rows()[row][c0])) {
        doc_.setCell(row, c0, matcher_->replace(doc_.rows()[row][c0], replaceBuffer_));
        markChanged();
    }
    findNext(true);
}

void CsvEditorTab::replaceAll() {
    find_.text = findBuffer_;
    int r0 = 0, c0 = 0, r1, c1;
    table_->getSelectionRange(r0, c0, r1, c1);
    find_.column = onlyCurrentColumn_ ? std::max(c0, 0) : -1;
    const CsvDocument::Matcher matches(find_);
    if (!matches.valid()) {
        findStatus_ = "Invalid regular expression";
        return;
    }
    int count = 0;
    if (!filtered_) {
        count = doc_.replaceAll(find_, replaceBuffer_);
    } else {
        std::vector<CsvDocument::CellValue> cells;
        for (int r : viewRows_)
            for (int c = 0; c < doc_.columnCount(); ++c)
                if ((find_.column < 0 || c == find_.column) && matches(doc_.rows()[r][c]))
                    cells.push_back({r, c, matches.replace(doc_.rows()[r][c], replaceBuffer_)});
        count = static_cast<int>(cells.size());
        doc_.setCells(cells);
    }
    findStatus_ =
        count == 0 ? "No matches" : std::format("Replaced {} cell{}", count, count == 1 ? "" : "s");
    if (count > 0)
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

void CsvEditorTab::jumpToFlag(bool forward) {
    const auto flagged = doc_.flaggedRows();
    std::vector<int> visible;
    for (int r : flagged)
        if (viewRowOf(r) >= 0)
            visible.push_back(r);
    if (visible.empty()) {
        statusText_ = "No flagged rows";
        return;
    }
    int r0 = -1, c0 = 0, r1, c1;
    const int current = table_->getSelectionRange(r0, c0, r1, c1) ? docRow(r0) : -1;
    int target;
    if (forward) {
        const auto it = std::ranges::upper_bound(visible, current);
        target = it == visible.end() ? visible.front() : *it;
    } else {
        const auto it = std::ranges::lower_bound(visible, current);
        target = it == visible.begin() ? visible.back() : *(it - 1);
    }
    selectCell(target, std::max(c0, 0));
}

void CsvEditorTab::exportJson() {
    const auto stem = std::filesystem::path(filePath_).stem().string();
    const std::string path = FileDialog::saveFile(stem + ".json", "JSON Files", "json");
    if (path.empty())
        return;
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out << doc_.toJson(filtered_ ? viewRows_ : std::vector<int>{});
    out.close();
    if (!out)
        showMessage("Could not write " + path);
    else
        statusText_ = std::format("Exported {} rows to JSON", viewRowCount());
}

void CsvEditorTab::exportFlagged() {
    const auto rows = doc_.flaggedRows();
    if (rows.empty()) {
        showMessage("There are no flagged rows.");
        return;
    }
    const auto stem = std::filesystem::path(filePath_).stem().string();
    const std::string path =
        FileDialog::saveFile(stem + "_flagged.csv", "CSV Files", "csv,tsv,txt");
    if (path.empty())
        return;
    std::string error;
    if (!doc_.saveRows(path, rows, error))
        showMessage("Could not export the flagged rows.\n\n" + error);
    else
        statusText_ = std::format("Exported {} flagged rows", rows.size());
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

    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);
    renderToolbar();
    ImGui::PopStyleColor(3);
    if (findOpen_)
        renderFindBar();

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
    const bool mod = io.KeyMods & ImGuiMod_Shortcut;
    if (!mod)
        return;
    const bool editingText = io.WantTextInput || table_->isEditing();

    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (io.KeyShift)
            saveAs();
        else
            save();
    } else if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        findOpen_ = true;
        focusFind_ = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        inputNumber_ = 1;
        dialogColumn_ = 0;
        openDialog(Dialog::JumpTo);
    } else if (!editingText && ((ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyShift) ||
                                ImGui::IsKeyPressed(ImGuiKey_Y, false))) {
        if (doc_.redo())
            markChanged();
    } else if (!editingText && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (doc_.undo())
            markChanged();
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
    if (UIUtils::IconButton(ICON_FA_ROTATE_LEFT) && doc_.undo())
        markChanged();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo (%s)", shortcut("Z").c_str());
    ImGui::SameLine(0, Theme::Spacing::XS);
    ImGui::BeginDisabled(!doc_.canRedo());
    if (UIUtils::IconButton(ICON_FA_ROTATE_RIGHT) && doc_.redo())
        markChanged();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo (%s)", shortcut("Z", true).c_str());

    // menus
    ImGui::SameLine(0, Theme::Spacing::M);
    if (UIUtils::Button("File " ICON_FA_CARET_DOWN))
        ImGui::OpenPopup("##csv_file_menu");
    renderFileMenu();
    ImGui::SameLine(0, Theme::Spacing::XS);
    if (UIUtils::Button("Edit " ICON_FA_CARET_DOWN))
        ImGui::OpenPopup("##csv_edit_menu");
    ImGui::SameLine(0, Theme::Spacing::XS);
    if (UIUtils::Button("Data " ICON_FA_CARET_DOWN))
        ImGui::OpenPopup("##csv_data_menu");
    renderEditMenu();
    renderDataMenu();

    ImGui::SameLine(0, Theme::Spacing::XS);
    if (UIUtils::IconButton(ICON_FA_MAGNIFYING_GLASS)) {
        findOpen_ = !findOpen_;
        focusFind_ = findOpen_;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Find and replace (%s)", shortcut("F").c_str());

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

void CsvEditorTab::renderFileMenu() {
    if (!UIUtils::BeginMenuPopup("##csv_file_menu"))
        return;
    // tabs render with zero item spacing; menus put it back, like the others
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    if (ImGui::MenuItem("Save", shortcut("S").c_str(), false, hasUnsavedChanges()))
        save();
    if (ImGui::MenuItem("Save As...", shortcut("S", true).c_str()))
        saveAs();
    if (ImGui::MenuItem("Reload from Disk")) {
        if (doc_.isModified())
            openDialog(Dialog::ConfirmReload);
        else
            startLoad();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("CSV Properties...")) {
        editDialect_ = doc_.dialect();
        copyTo(inputBuffer_, sizeof(inputBuffer_), editDialect_.delimiter);
        openDialog(Dialog::Properties);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Export JSON..."))
        exportJson();
    if (ImGui::MenuItem("Export Flagged Rows...", nullptr, false, doc_.flaggedCount() > 0))
        exportFlagged();
    if (ImGui::MenuItem("Split File...", nullptr, false, doc_.rowCount() > 0)) {
        inputNumber_ = 1000;
        openDialog(Dialog::SplitFile);
    }
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

void CsvEditorTab::renderEditMenu() {
    if (!UIUtils::BeginMenuPopup("##csv_edit_menu"))
        return;
    // tabs render with zero item spacing; menus put it back, like the others
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    int r0 = -1, c0 = -1, r1 = -1, c1 = -1;
    const bool hasSel = table_->getSelectionRange(r0, c0, r1, c1);
    const auto rows = selectedDocRows();

    if (ImGui::MenuItem("Undo", shortcut("Z").c_str(), false, doc_.canUndo()) && doc_.undo())
        markChanged();
    if (ImGui::MenuItem("Redo", shortcut("Z", true).c_str(), false, doc_.canRedo()) && doc_.redo())
        markChanged();
    ImGui::Separator();

    // inserting rows into a filtered view would land them out of sight
    if (ImGui::MenuItem("Insert Row Above", nullptr, false, !filtered_)) {
        doc_.insertRows(hasSel ? docRow(r0) : 0);
        markChanged();
    }
    if (ImGui::MenuItem("Insert Row Below", nullptr, false, !filtered_)) {
        doc_.insertRows(hasSel ? docRow(r1) + 1 : doc_.rowCount());
        markChanged();
    }
    if (ImGui::MenuItem(rows.size() > 1 ? "Delete Rows" : "Delete Row", nullptr, false,
                        !rows.empty()))
        deleteDocRows(rows);
    ImGui::Separator();
    if (ImGui::MenuItem("Insert Column Left", nullptr, false, true)) {
        doc_.insertColumn(hasSel ? c0 : 0);
        markChanged();
    }
    if (ImGui::MenuItem("Insert Column Right", nullptr, false, true)) {
        doc_.insertColumn(hasSel ? c1 + 1 : doc_.columnCount());
        markChanged();
    }
    if (ImGui::MenuItem(c1 > c0 ? "Delete Columns" : "Delete Column", nullptr, false, hasSel)) {
        doc_.deleteColumns(c0, c1);
        table_->setSelectedCell(-1, -1);
        markChanged();
    }
    if (ImGui::MenuItem("Move Columns Left", nullptr, false, hasSel && c0 > 0) &&
        doc_.moveColumns(c0, c1, -1)) {
        markChanged();
        table_->setSelectedCell(-1, -1);
    }
    if (ImGui::MenuItem("Move Columns Right", nullptr, false,
                        hasSel && c1 < doc_.columnCount() - 1) &&
        doc_.moveColumns(c0, c1, 1)) {
        markChanged();
        table_->setSelectedCell(-1, -1);
    }
    if (ImGui::MenuItem("Rename Column...", nullptr, false, hasSel && doc_.dialect().hasHeader)) {
        dialogColumn_ = c0;
        copyTo(inputBuffer_, sizeof(inputBuffer_), doc_.headers()[c0]);
        openDialog(Dialog::RenameColumn);
    }
    ImGui::Separator();
    bool header = doc_.dialect().hasHeader;
    if (ImGui::MenuItem("First Row Is Header", nullptr, &header, !filtered_)) {
        doc_.setHasHeader(header);
        filterInput_->addKeywords(doc_.headers());
        markChanged();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Find and Replace...", shortcut("F").c_str())) {
        findOpen_ = true;
        focusFind_ = true;
    }
    if (ImGui::MenuItem("Jump to Cell...", shortcut("L").c_str())) {
        inputNumber_ = hasSel ? docRow(r0) + 1 : 1;
        dialogColumn_ = hasSel ? c0 : 0;
        openDialog(Dialog::JumpTo);
    }
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

void CsvEditorTab::renderDataMenu() {
    if (!UIUtils::BeginMenuPopup("##csv_data_menu"))
        return;
    // tabs render with zero item spacing; menus put it back, like the others
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    int r0 = -1, c0 = -1, r1 = -1, c1 = -1;
    const bool hasSel = table_->getSelectionRange(r0, c0, r1, c1);
    const auto rows = selectedDocRows();
    const int flagged = doc_.flaggedCount();

    if (ImGui::MenuItem("Sort Ascending by Column", nullptr, false, hasSel))
        sortColumn(c0, true);
    if (ImGui::MenuItem("Sort Descending by Column", nullptr, false, hasSel))
        sortColumn(c0, false);
    ImGui::Separator();
    if (ImGui::MenuItem("Split Column...", nullptr, false, hasSel)) {
        dialogColumn_ = c0;
        copyTo(inputBuffer_, sizeof(inputBuffer_), ",");
        openDialog(Dialog::SplitColumn);
    }
    if (ImGui::MenuItem("Merge Selected Columns...", nullptr, false, hasSel && c1 > c0)) {
        inputNumber_ = c0;
        inputNumber2_ = c1;
        copyTo(inputBuffer_, sizeof(inputBuffer_), " ");
        openDialog(Dialog::MergeColumns);
    }
    if (ImGui::MenuItem("Check Column Consistency", nullptr, false, hasSel)) {
        const int count = doc_.flagInconsistent(c0);
        showMessage(count == 0
                        ? std::format("Every value in \"{}\" looks alike.", doc_.headers()[c0])
                        : std::format("{} row{} in \"{}\" do not match the column's "
                                      "usual kind of value and were flagged.",
                                      count, count == 1 ? "" : "s", doc_.headers()[c0]));
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Flag Selected Rows", nullptr, false, !rows.empty()))
        doc_.setFlags(rows, true);
    if (ImGui::MenuItem("Unflag Selected Rows", nullptr, false, !rows.empty()))
        doc_.setFlags(rows, false);
    if (ImGui::MenuItem("Invert Flags"))
        doc_.invertFlags();
    if (ImGui::MenuItem("Clear Flags", nullptr, false, flagged > 0))
        doc_.clearFlags();
    if (ImGui::MenuItem("Previous Flagged Row", nullptr, false, flagged > 0))
        jumpToFlag(false);
    if (ImGui::MenuItem("Next Flagged Row", nullptr, false, flagged > 0))
        jumpToFlag(true);
    if (ImGui::MenuItem("Delete Flagged Rows", nullptr, false, flagged > 0))
        deleteDocRows(doc_.flaggedRows());
    if (ImGui::MenuItem("Export Flagged Rows...", nullptr, false, flagged > 0))
        exportFlagged();
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

void CsvEditorTab::renderColumnMenu(int col) {
    if (ImGui::MenuItem("Insert Column Left")) {
        doc_.insertColumn(col);
        markChanged();
    }
    if (ImGui::MenuItem("Insert Column Right")) {
        doc_.insertColumn(col + 1);
        markChanged();
    }
    if (ImGui::MenuItem("Rename...", nullptr, false, doc_.dialect().hasHeader)) {
        dialogColumn_ = col;
        copyTo(inputBuffer_, sizeof(inputBuffer_), doc_.headers()[col]);
        openDialog(Dialog::RenameColumn);
    }
    if (ImGui::MenuItem("Move Left", nullptr, false, col > 0) && doc_.moveColumns(col, col, -1))
        markChanged();
    if (ImGui::MenuItem("Move Right", nullptr, false, col < doc_.columnCount() - 1) &&
        doc_.moveColumns(col, col, 1))
        markChanged();
    if (ImGui::MenuItem("Split...")) {
        dialogColumn_ = col;
        copyTo(inputBuffer_, sizeof(inputBuffer_), ",");
        openDialog(Dialog::SplitColumn);
    }
    if (ImGui::MenuItem("Check Consistency")) {
        const int count = doc_.flagInconsistent(col);
        showMessage(count == 0 ? "Every value in this column looks alike."
                               : std::format("{} inconsistent row{} flagged.", count,
                                             count == 1 ? "" : "s"));
    }
    if (ImGui::MenuItem("Delete Column")) {
        doc_.deleteColumns(col, col);
        table_->setSelectedCell(-1, -1);
        markChanged();
    }
}

void CsvEditorTab::renderCellMenu(int row, int) {
    auto rows = selectedDocRows();
    if (rows.empty())
        rows.push_back(docRow(row));
    const bool flagged = doc_.isFlagged(docRow(row));
    if (ImGui::MenuItem(flagged ? ICON_FA_FLAG " Unflag Row" : ICON_FA_FLAG " Flag Row"))
        doc_.setFlags(rows, !flagged);
    if (ImGui::MenuItem("Insert Row Above", nullptr, false, !filtered_)) {
        doc_.insertRows(docRow(row));
        markChanged();
    }
    if (ImGui::MenuItem("Insert Row Below", nullptr, false, !filtered_)) {
        doc_.insertRows(docRow(row) + 1);
        markChanged();
    }
}

void CsvEditorTab::renderFindBar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    bool changed = false;

    ImGui::SetNextItemWidth(240.0f);
    if (focusFind_) {
        ImGui::SetKeyboardFocusHere();
        focusFind_ = false;
    }
    if (ImGui::InputTextWithHint("##csv_find", "Find", findBuffer_, sizeof(findBuffer_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        findNext(!ImGui::GetIO().KeyShift);
        ImGui::SetKeyboardFocusHere(-1);
    }
    changed |= ImGui::IsItemEdited();
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        findOpen_ = false;

    auto toggle = [&](const char* label, bool& value, const char* tip) {
        ImGui::SameLine(0, Theme::Spacing::XS);
        if (value)
            ImGui::PushStyleColor(ImGuiCol_Button, colors.surface2);
        if (UIUtils::Button(label, value ? UIUtils::ButtonVariant::Custom
                                         : UIUtils::ButtonVariant::Secondary)) {
            value = !value;
            changed = true;
        }
        if (value)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
    };
    toggle("Aa", find_.matchCase, "Match case");
    toggle("[ab]", find_.wholeCell, "Match the whole cell");
    toggle(".*", find_.regex, "Regular expression");
    toggle(ICON_FA_TABLE_COLUMNS, onlyCurrentColumn_, "Only the selected column");

    ImGui::SameLine(0, Theme::Spacing::S);
    if (UIUtils::IconButton(ICON_FA_ARROW_UP))
        findNext(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Previous match (Shift+Enter)");
    ImGui::SameLine(0, Theme::Spacing::XS);
    if (UIUtils::IconButton(ICON_FA_ARROW_DOWN))
        findNext(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Next match (Enter)");

    ImGui::SameLine(0, Theme::Spacing::M);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##csv_replace", "Replace with", replaceBuffer_,
                             sizeof(replaceBuffer_));
    ImGui::SameLine(0, Theme::Spacing::XS);
    if (UIUtils::Button("Replace"))
        replaceCurrent();
    ImGui::SameLine(0, Theme::Spacing::XS);
    if (UIUtils::Button("Replace All"))
        replaceAll();

    if (!findStatus_.empty()) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(colors.subtext0, "%s", findStatus_.c_str());
    }
    ImGui::SameLine(0, Theme::Spacing::M);
    if (UIUtils::IconButton(ICON_FA_XMARK))
        findOpen_ = false;

    if (changed) {
        find_.text = findBuffer_;
        findStatus_.clear();
    }
    // the matcher behind the cell highlight; rebuilt only when the search changes
    if (changed || !matcher_) {
        find_.text = findBuffer_;
        matcher_.emplace(find_);
    }
}

void CsvEditorTab::renderTableView() {
    if (tableDirty_)
        rebuildTable();
    if (doc_.columnCount() == 0) {
        ImGui::TextDisabled("This file is empty.");
        if (UIUtils::Button(ICON_FA_PLUS " Add Column")) {
            doc_.insertColumn(0, "column1");
            markChanged();
        }
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
    if (const int flagged = doc_.flaggedCount(); flagged > 0)
        text += std::format(" · {} flagged", flagged);
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
    const auto& colors = Application::getInstance().getCurrentColors();
    bool close = false;
    auto title = [&](const char* text) {
        ImGui::TextUnformatted(text);
        ImGui::Spacing();
    };
    auto buttons = [&](const char* ok, bool enabled = true) {
        ImGui::Spacing();
        bool pressed = false;
        ImGui::BeginDisabled(!enabled);
        if (UIUtils::Button(ok, UIUtils::ButtonVariant::Primary) ||
            (enabled && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
            pressed = true;
        ImGui::EndDisabled();
        ImGui::SameLine(0, Theme::Spacing::S);
        if (UIUtils::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
        if (pressed)
            close = true;
        return pressed;
    };
    auto column = [&](int col) {
        return col >= 0 && col < doc_.columnCount() ? doc_.headers()[col] : std::string{};
    };

    switch (dialog_) {
    case Dialog::Message:
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(message_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (UIUtils::Button("OK", UIUtils::ButtonVariant::Primary) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
        break;

    case Dialog::ConfirmReload:
        title("Discard unsaved changes and reload the file?");
        if (buttons("Reload"))
            startLoad();
        else if (close)
            reloadDialect_.reset();
        break;

    case Dialog::RenameColumn:
        title("Rename column");
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##rename", inputBuffer_, sizeof(inputBuffer_));
        if (buttons("Rename")) {
            doc_.renameColumn(dialogColumn_, inputBuffer_);
            filterInput_->addKeywords({inputBuffer_});
            markChanged();
        }
        break;

    case Dialog::SplitColumn:
        ImGui::Text("Split \"%s\" into columns", column(dialogColumn_).c_str());
        ImGui::Spacing();
        ImGui::TextColored(colors.subtext0, "Separator");
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##sep", inputBuffer_, sizeof(inputBuffer_));
        if (buttons("Split", inputBuffer_[0] != '\0')) {
            const int parts = doc_.splitColumn(dialogColumn_, inputBuffer_);
            if (parts == 0)
                statusText_ = "Nothing to split";
            else {
                statusText_ = std::format("Split into {} columns", parts);
                markChanged();
            }
        }
        break;

    case Dialog::MergeColumns:
        ImGui::Text("Merge \"%s\" to \"%s\" into one column", column(inputNumber_).c_str(),
                    column(inputNumber2_).c_str());
        ImGui::Spacing();
        ImGui::TextColored(colors.subtext0, "Join with");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##join", inputBuffer_, sizeof(inputBuffer_));
        if (buttons("Merge")) {
            doc_.mergeColumns(inputNumber_, inputNumber2_, inputBuffer_);
            table_->setSelectedCell(-1, -1);
            markChanged();
        }
        break;

    case Dialog::SplitFile: {
        title("Split into several files");
        ImGui::TextColored(colors.subtext0, "Rows per file");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputInt("##rows", &inputNumber_, 100, 1000);
        inputNumber_ = std::max(inputNumber_, 1);
        const int files = (doc_.rowCount() + inputNumber_ - 1) / inputNumber_;
        ImGui::TextColored(colors.subtext0, "%d file%s, each with the header row", files,
                           files == 1 ? "" : "s");
        if (buttons("Choose Location...")) {
            const auto stem = std::filesystem::path(filePath_).filename().string();
            const std::string path = FileDialog::saveFile(stem, "CSV Files", "csv,tsv,txt");
            if (!path.empty()) {
                std::string error;
                const auto written = doc_.splitFile(path, inputNumber_, error);
                if (!error.empty())
                    message_ = "Could not split the file.\n\n" + error;
                else
                    statusText_ = std::format("Wrote {} files", written.size());
            }
        }
        break;
    }

    case Dialog::JumpTo: {
        title("Jump to cell");
        ImGui::TextColored(colors.subtext0, "Row (1 to %d)", doc_.rowCount());
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputInt("##row", &inputNumber_);
        inputNumber_ = std::clamp(inputNumber_, 1, std::max(doc_.rowCount(), 1));
        ImGui::TextColored(colors.subtext0, "Column");
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##col", column(dialogColumn_).c_str())) {
            for (int c = 0; c < doc_.columnCount(); ++c) {
                ImGui::PushID(c);
                if (ImGui::Selectable(doc_.headers()[c].empty() ? "(unnamed)"
                                                                : doc_.headers()[c].c_str(),
                                      c == dialogColumn_))
                    dialogColumn_ = c;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (buttons("Go")) {
            const int row = inputNumber_ - 1;
            if (viewRowOf(row) < 0 && filtered_)
                clearFilter();
            selectCell(row, std::max(dialogColumn_, 0));
        }
        break;
    }

    case Dialog::Properties: {
        title("CSV properties");
        CsvDialect& d = editDialect_;
        const float w = 220.0f;

        int delim = 5;
        for (int i = 0; i < 5; ++i)
            if (d.delimiter == kDelimiters[i])
                delim = i;
        ImGui::TextColored(colors.subtext0, "Delimiter");
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##delim", &delim, kDelimiterLabels, 6)) {
            if (delim < 5)
                d.delimiter = kDelimiters[delim];
            copyTo(inputBuffer_, sizeof(inputBuffer_), d.delimiter);
        }
        if (delim == 5) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            if (ImGui::InputText("##delim_other", inputBuffer_, sizeof(inputBuffer_)) &&
                inputBuffer_[0])
                d.delimiter = inputBuffer_;
        }

        ImGui::TextColored(colors.subtext0, "Quote character");
        char quote[2] = {d.quote, '\0'};
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::InputText("##quote", quote, sizeof(quote)) && quote[0])
            d.quote = quote[0];

        ImGui::TextColored(colors.subtext0, "Quote fields");
        int style = static_cast<int>(d.quoteStyle);
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##qstyle", &style, kQuoteStyleLabels, 3))
            d.quoteStyle = static_cast<CsvDialect::QuoteStyle>(style);

        ImGui::TextColored(colors.subtext0, "Line break");
        int lb = d.lineBreak == "\r\n" ? 1 : d.lineBreak == "\r" ? 2 : 0;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##lb", &lb, kLineBreakLabels, 3))
            d.lineBreak = kLineBreaks[lb];

        ImGui::TextColored(colors.subtext0, "Encoding");
        int enc = static_cast<int>(d.encoding);
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##enc", &enc, kEncodingLabels, 4))
            d.encoding = static_cast<CsvDialect::Encoding>(enc);
        ImGui::SameLine();
        ImGui::Checkbox("BOM", &d.bom);

        ImGui::Checkbox("First row is the header", &d.hasHeader);
        ImGui::Checkbox("End the file with a line break", &d.trailingLineBreak);

        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextColored(colors.subtext0,
                           "Apply keeps the data and writes it in this format on the next "
                           "save. Reopen reads the file again with these settings, for a "
                           "file that was detected wrongly.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (UIUtils::Button("Apply", UIUtils::ButtonVariant::Primary)) {
            doc_.setDialect(d);
            doc_.setHasHeader(d.hasHeader);
            filterInput_->addKeywords(doc_.headers());
            markChanged();
            close = true;
        }
        ImGui::SameLine(0, Theme::Spacing::S);
        if (UIUtils::Button("Reopen with These Settings")) {
            reloadDialect_ = d;
            close = true;
            if (doc_.isModified()) {
                // keep the chosen format for after the confirmation
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::PopStyleVar();
                openDialog(Dialog::ConfirmReload);
                return;
            }
            startLoad();
        }
        ImGui::SameLine(0, Theme::Spacing::S);
        if (UIUtils::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;
        break;
    }

    case Dialog::None:
        close = true;
        break;
    }

    if (close) {
        ImGui::CloseCurrentPopup();
        // a dialog that ended in an error shows it next
        if (!message_.empty() && dialog_ != Dialog::Message) {
            ImGui::EndPopup();
            ImGui::PopStyleVar();
            openDialog(Dialog::Message);
            return;
        }
        if (dialog_ == Dialog::Message)
            message_.clear();
        dialog_ = Dialog::None;
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar();
}
