#include "ui/tab/csv_editor_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/csv_parser.hpp"
#include <filesystem>
#include <format>
#include <spdlog/spdlog.h>

CsvEditorTab::CsvEditorTab(const std::string& name, std::string filePath)
    : Tab(name, TabType::CSV_EDITOR), filePath_(std::move(filePath)) {
    rawEditor_.SetLanguage(dearsql::TextEditor::Language::PlainText);
    rawEditor_.SetShowLineNumbers(true);
    rawEditor_.SetTabSize(4);
    loadFile();
    initTableRenderer();
}

void CsvEditorTab::clearValidationError() {
    hasValidationError_ = false;
    validationError_.clear();
}

void CsvEditorTab::setValidationError(std::string message) {
    hasValidationError_ = true;
    validationError_ = std::move(message);
    spdlog::warn(validationError_);
}

bool CsvEditorTab::hasPendingChanges() const {
    const bool tableEditPending =
        viewMode_ == ViewMode::Table && tableRenderer_ && tableRenderer_->isEditing();
    const bool rawEditPending =
        viewMode_ == ViewMode::Raw && (rawDirty_ || rawEditor_.IsContentDirty());
    return hasChanges_ || tableEditPending || rawEditPending;
}

bool CsvEditorTab::hasUnsavedChanges() const {
    return hasPendingChanges();
}

void CsvEditorTab::loadFile() {
    loadError_ = false;
    errorMessage_.clear();
    clearValidationError();

    if (!CsvParser::parseFile(filePath_, headers_, rows_)) {
        loadError_ = true;
        errorMessage_ = "Failed to open file: " + filePath_;
        spdlog::error(errorMessage_);
        return;
    }

    syncTableToRaw();
    tableRendererDataDirty_ = true;
    hasChanges_ = false;
    rawDirty_ = false;
    spdlog::debug("CSV loaded: {} headers, {} rows", headers_.size(), rows_.size());
}

void CsvEditorTab::saveFile() {
    if (viewMode_ == ViewMode::Table && tableRenderer_ && tableRenderer_->isEditing()) {
        tableRenderer_->exitEditMode(true);
    }

    // always sync raw editor → table when in raw view
    if (viewMode_ == ViewMode::Raw) {
        if (!syncRawToTable())
            return;
    }

    if (!CsvParser::writeFile(filePath_, headers_, rows_)) {
        spdlog::error("Failed to save CSV file: {}", filePath_);
        return;
    }

    hasChanges_ = false;
    rawDirty_ = false;
    syncTableToRaw();
    spdlog::debug("CSV saved: {}", filePath_);
}

void CsvEditorTab::syncTableToRaw() {
    rawEditor_.SetText(CsvParser::serialize(headers_, rows_));
}

bool CsvEditorTab::syncRawToTable() {
    const std::string text = rawEditor_.GetText();
    std::vector<std::string> newHeaders;
    std::vector<std::vector<std::string>> newRows;

    if (!CsvParser::parseText(text, newHeaders, newRows)) {
        setValidationError("Raw CSV content is invalid and could not be parsed.");
        return false;
    }

    headers_ = std::move(newHeaders);
    rows_ = std::move(newRows);
    tableRendererDataDirty_ = true;
    rawDirty_ = false;
    clearValidationError();
    return true;
}

void CsvEditorTab::initTableRenderer() {
    TableRenderer::Config cfg;
    cfg.allowEditing = true;
    cfg.allowSelection = true;
    cfg.showRowNumbers = true;
    cfg.minHeight = 200.0f;

    tableRenderer_ = std::make_unique<TableRenderer>(cfg);

    tableRenderer_->setOnCellEdit([this](int row, int col, const std::string& newValue) {
        if (row < static_cast<int>(rows_.size()) && col < static_cast<int>(headers_.size())) {
            if (rows_[row][col] != newValue) {
                rows_[row][col] = newValue;
                tableRendererDataDirty_ = true;
                hasChanges_ = true;
            }
        }
    });

    tableRenderer_->setOnCellSelect([this](int row, int col) {
        selectedRow_ = row;
        selectedCol_ = col;
    });

    syncTableRendererData();
}

void CsvEditorTab::syncTableRendererData() {
    if (!tableRenderer_)
        return;

    tableRenderer_->setColumns(headers_);
    tableRenderer_->setData(rows_);
    tableRendererDataDirty_ = false;
}

void CsvEditorTab::render() {
    const auto& colors = Application::getInstance().getCurrentColors();

    // Cmd+S / Ctrl+S to save
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (hasPendingChanges())
            saveFile();
    }

    renderToolbar();
    ImGui::Separator();

    if (loadError_) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("%s", errorMessage_.c_str());
        ImGui::PopStyleColor();
        return;
    }

    if (hasValidationError_) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        ImGui::TextWrapped("%s", validationError_.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (viewMode_ == ViewMode::Table) {
        renderTableView();
    } else {
        renderRawView();
    }
}

void CsvEditorTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    const bool canSave = hasPendingChanges();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.surface2);

    // file path (truncated)
    const std::string filename = std::filesystem::path(filePath_).filename().string();
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::Text("%s %s", ICON_FA_FILE_CSV, filename.c_str());
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", filePath_.c_str());
    }

    ImGui::SameLine(0, Theme::Spacing::M);

    if (canSave) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        ImGui::TextUnformatted("Edited");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, Theme::Spacing::M);
    }

    // view mode toggle
    const bool tableActive = (viewMode_ == ViewMode::Table);
    const bool rawActive = (viewMode_ == ViewMode::Raw);

    if (tableActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    }
    if (ImGui::Button(ICON_FA_TABLE " Table")) {
        if (viewMode_ == ViewMode::Raw && rawDirty_) {
            if (!syncRawToTable())
                return;
        }
        viewMode_ = ViewMode::Table;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Table view");

    ImGui::SameLine(0, Theme::Spacing::XS);

    if (rawActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface0);
    }
    if (ImGui::Button(ICON_FA_CODE " Raw")) {
        if (viewMode_ == ViewMode::Table) {
            if (tableRenderer_ && tableRenderer_->isEditing()) {
                tableRenderer_->exitEditMode(true);
            }
            syncTableToRaw();
        }
        viewMode_ = ViewMode::Raw;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Raw text view");

    ImGui::SameLine(0, Theme::Spacing::M);

    // stats
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::Text("%d rows · %d cols", static_cast<int>(rows_.size()),
                static_cast<int>(headers_.size()));
    ImGui::PopStyleColor();

    ImGui::SameLine(0, Theme::Spacing::M);

    // save button
    if (canSave) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.green);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK)) {
            saveFile();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save");

        ImGui::SameLine(0, Theme::Spacing::XS);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        if (ImGui::Button(ICON_FA_XMARK)) {
            if (tableRenderer_ && tableRenderer_->isEditing()) {
                tableRenderer_->exitEditMode(false);
            }
            loadFile();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Revert changes");

        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.peach);
        ImGui::TextUnformatted("Unsaved changes");
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_FLOPPY_DISK);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Save");
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}

void CsvEditorTab::renderTableView() {
    if (headers_.empty()) {
        ImGui::TextDisabled("No data.");
        return;
    }

    if (tableRendererDataDirty_)
        syncTableRendererData();

    tableRenderer_->setSelectedCell(selectedRow_, selectedCol_);

    tableRenderer_->render("CsvTable");

    // keyboard navigation
    if (selectedRow_ >= 0 && selectedCol_ >= 0 && !tableRenderer_->isEditing()) {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            return;
        if (ImGui::GetIO().WantTextInput)
            return;

        const int maxRows = static_cast<int>(rows_.size());
        const int maxCols = static_cast<int>(headers_.size());

        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && selectedRow_ > 0)
            selectedRow_--;
        else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && selectedRow_ < maxRows - 1)
            selectedRow_++;
        else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && selectedCol_ > 0)
            selectedCol_--;
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && selectedCol_ < maxCols - 1)
            selectedCol_++;
    }
}

void CsvEditorTab::renderRawView() {
    const auto& app = Application::getInstance();
    const bool darkTheme = app.isDarkTheme();

    // sync palette to current theme only when it changes
    if (!rawPaletteSet_ || darkTheme != rawLastDark_) {
        rawEditor_.SetPalette(dearsql::TextEditor::FromTheme(app.getCurrentColors()));
        rawPaletteSet_ = true;
        rawLastDark_ = darkTheme;
    }

    const ImVec2 size(-1.0f, ImGui::GetContentRegionAvail().y);
    rawEditor_.Render("##csv_raw", size, false);

    // sample dirty state after rendering so edits made this frame are visible immediately
    if (rawEditor_.IsContentDirty()) {
        rawEditor_.ClearContentDirty();
        hasChanges_ = true;
        rawDirty_ = true;
        clearValidationError();
    }
}
