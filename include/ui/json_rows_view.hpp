#pragma once

#include "application.hpp"
#include "ui/text_editor.hpp"
#include "utils/table_exporter.hpp"

// a grid's rows as read-only json; rebuilt when the rows change or invalidate() is called
class JsonRowsView {
public:
    JsonRowsView() {
        editor_.SetLanguage(dearsql::TextEditor::Language::JSON);
        editor_.SetReadOnly(true);
    }

    // for tests: drawn this frame or the last, and what it shows
    [[nodiscard]] bool visible() const {
        return lastFrame_ >= ImGui::GetFrameCount() - 1;
    }
    [[nodiscard]] std::string text() const {
        return editor_.GetText();
    }

    void invalidate() {
        rows_ = nullptr;
    }

    void render(const char* id, const std::vector<std::string>& columns,
                const std::vector<std::vector<std::string>>& rows) {
        // ponytail: change detection by buffer and size; in-place cell edits need invalidate()
        if (rows_ != rows.data() || size_ != rows.size()) {
            editor_.SetText(TableExporter::rowsToJson(columns, rows));
            rows_ = rows.data();
            size_ = rows.size();
        }
        editor_.SetPalette(
            dearsql::TextEditor::FromTheme(Application::getInstance().getCurrentColors()));
        lastFrame_ = ImGui::GetFrameCount();
        ImGui::PushID(id); // the editor ignores its label
        editor_.Render(id, ImVec2(-1, -1), false);
        ImGui::PopID();
    }

private:
    dearsql::TextEditor editor_;
    const std::vector<std::string>* rows_ = nullptr;
    size_t size_ = 0;
    int lastFrame_ = -10;
};
