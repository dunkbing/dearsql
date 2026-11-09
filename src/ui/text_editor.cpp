#include "ui/text_editor.hpp"
#include "IconsFontAwesome6.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>

namespace dearsql {

    TextEditor::TextEditor() {
        palette_ = GetDarkPalette();
        lineStarts_.push_back(0);
        initTreeSitter();
    }

    TextEditor::~TextEditor() {
        cleanupTreeSitter();
    }

    TextEditor::TextEditor(TextEditor&& other) noexcept
        : content_(std::move(other.content_)), colors_(std::move(other.colors_)),
          lineStarts_(std::move(other.lineStarts_)), cursorIndex_(other.cursorIndex_),
          preferredColumn_(other.preferredColumn_), cursorBlinkTimer_(other.cursorBlinkTimer_),
          selectionAnchor_(other.selectionAnchor_), selectionActive_(other.selectionActive_),
          isDragging_(other.isDragging_), scrollX_(other.scrollX_), scrollY_(other.scrollY_),
          showLineNumbers_(other.showLineNumbers_), tabSize_(other.tabSize_),
          readOnly_(other.readOnly_), focusRequested_(other.focusRequested_),
          palette_(other.palette_), undoStack_(std::move(other.undoStack_)),
          redoStack_(std::move(other.redoStack_)),
          lastSnapshotContent_(std::move(other.lastSnapshotContent_)),
          completionItems_(std::move(other.completionItems_)),
          completionFilter_(std::move(other.completionFilter_)),
          completionProvider_(std::move(other.completionProvider_)),
          rangeProvider_(std::move(other.rangeProvider_)), onRun_(std::move(other.onRun_)),
          language_(other.language_), tsParser_(other.tsParser_), tsTree_(other.tsTree_),
          tsQuery_(other.tsQuery_), tsPreviousContent_(std::move(other.tsPreviousContent_)),
          tsPreviousColors_(std::move(other.tsPreviousColors_)),
          highlightDirty_(other.highlightDirty_) {
        other.tsParser_ = nullptr;
        other.tsTree_ = nullptr;
        other.tsQuery_ = nullptr;
    }

    TextEditor& TextEditor::operator=(TextEditor&& other) noexcept {
        if (this != &other) {
            cleanupTreeSitter();
            content_ = std::move(other.content_);
            colors_ = std::move(other.colors_);
            lineStarts_ = std::move(other.lineStarts_);
            cursorIndex_ = other.cursorIndex_;
            preferredColumn_ = other.preferredColumn_;
            palette_ = other.palette_;
            undoStack_ = std::move(other.undoStack_);
            redoStack_ = std::move(other.redoStack_);
            completionItems_ = std::move(other.completionItems_);
            completionFilter_ = std::move(other.completionFilter_);
            completionProvider_ = std::move(other.completionProvider_);
            rangeProvider_ = std::move(other.rangeProvider_);
            onRun_ = std::move(other.onRun_);
            runRangesDirty_ = true;
            language_ = other.language_;
            tsParser_ = other.tsParser_;
            tsTree_ = other.tsTree_;
            tsQuery_ = other.tsQuery_;
            tsPreviousContent_ = std::move(other.tsPreviousContent_);
            tsPreviousColors_ = std::move(other.tsPreviousColors_);
            highlightDirty_ = other.highlightDirty_;
            other.tsParser_ = nullptr;
            other.tsTree_ = nullptr;
            other.tsQuery_ = nullptr;
        }
        return *this;
    }

    void TextEditor::SetLanguage(Language lang) {
        if (language_ == lang)
            return;
        language_ = lang;
        cleanupTreeSitter();
        tsPreviousContent_.clear();
        initTreeSitter();
        highlightDirty_ = true;
    }

    void TextEditor::SetPlaceholder(const std::string& text) {
        placeholder_ = text;
    }

    void TextEditor::SetText(const std::string& text) {
        content_ = text;
        cursorIndex_ = static_cast<int>(text.size());
        selectionActive_ = false;
        selectionAnchor_ = cursorIndex_;
        rebuildLineStarts();
        contentDirty_ = false; // programmatic set is not a user edit
        highlightDirty_ = true;
        undoStack_.clear();
        redoStack_.clear();
        lastSnapshotContent_ = content_;
    }

    std::string TextEditor::GetText() const {
        return content_;
    }

    void TextEditor::SetFocus() {
        focusRequested_ = true;
    }

    void TextEditor::SetSubmitCallback(std::function<void()> cb) {
        submitCallback_ = std::move(cb);
    }

    void TextEditor::SetCompletionItems(const std::vector<CompletionItem>& items) {
        completionItems_ = items;
        for (auto& item : completionItems_) {
            if (item.insertText.empty())
                item.insertText = item.text;
            if (item.matchText.empty())
                item.matchText = item.text;
        }
    }

    void TextEditor::SetCompletionItems(std::vector<CompletionItem>&& items) {
        completionItems_ = std::move(items);
        for (auto& item : completionItems_) {
            if (item.insertText.empty())
                item.insertText = item.text;
            if (item.matchText.empty())
                item.matchText = item.text;
        }
    }

    void TextEditor::SetCompletionKeywords(const std::vector<std::string>& keywords) {
        completionItems_.clear();
        completionItems_.reserve(keywords.size());
        for (const auto& kw : keywords)
            completionItems_.push_back({kw, CompletionKind::Keyword});
    }

    void TextEditor::SetCompletionFilter(CompletionFilter filter) {
        completionFilter_ = std::move(filter);
    }

    void TextEditor::SetCompletionProvider(CompletionProvider provider) {
        completionProvider_ = std::move(provider);
    }

    void TextEditor::SetRunnableRanges(RangeProvider provider, RunCallback onRun) {
        rangeProvider_ = std::move(provider);
        onRun_ = std::move(onRun);
        runRangesDirty_ = true;
    }

    const std::vector<std::string>& TextEditor::GetDefaultCompletionKeywords() {
        static const std::vector<std::string> keywords = {
            "SELECT",
            "FROM",
            "WHERE",
            "AND",
            "OR",
            "NOT",
            "IN",
            "IS",
            "NULL",
            "AS",
            "ON",
            "JOIN",
            "LEFT",
            "RIGHT",
            "INNER",
            "OUTER",
            "CROSS",
            "FULL",
            "NATURAL",
            "GROUP",
            "BY",
            "ORDER",
            "ASC",
            "DESC",
            "HAVING",
            "LIMIT",
            "OFFSET",
            "UNION",
            "ALL",
            "DISTINCT",
            "BETWEEN",
            "LIKE",
            "EXISTS",
            "CASE",
            "WHEN",
            "THEN",
            "ELSE",
            "END",
            "BEGIN",
            "COMMIT",
            "ROLLBACK",
            "WITH",
            "RECURSIVE",
            "RETURNING",
            "CASCADE",
            "RESTRICT",
            "REFERENCES",
            "PRIMARY",
            "KEY",
            "FOREIGN",
            "UNIQUE",
            "CHECK",
            "DEFAULT",
            "CONSTRAINT",
            "IF",
            "REPLACE",
            "TEMPORARY",
            "INTO",
            "VALUES",
            "SET",
            "INSERT",
            "UPDATE",
            "DELETE",
            "CREATE",
            "DROP",
            "ALTER",
            "TABLE",
            "INDEX",
            "VIEW",
            "DATABASE",
            "SCHEMA",
            "TRIGGER",
            "FUNCTION",
            "RETURNS",
            "DECLARE",
            "OVER",
            "PARTITION",
            "WINDOW",
            "FILTER",
            "EXCEPT",
            "INTERSECT",
            "TRUE",
            "FALSE",
            "USING",
            "MATERIALIZED",
            "SEQUENCE",
            "TRUNCATE",
            "EXECUTE",
            "RETURN",
            "TRANSACTION",
            "EXPLAIN",
            "ANALYZE",
            "VACUUM",
            "GRANT",
            "REVOKE",
            "COUNT",
            "SUM",
            "AVG",
            "MIN",
            "MAX",
            "COALESCE",
            "NULLIF",
            "CAST",
            "EXTRACT",
            "SUBSTRING",
            "TRIM",
            "UPPER",
            "LOWER",
            "LENGTH",
            "CONCAT",
            "REPLACE",
            "NOW",
            "CURRENT_TIMESTAMP",
            "CURRENT_DATE",
            "CURRENT_TIME",
            "INTERVAL",
            "DATE",
            "TIME",
            "TIMESTAMP",
            "INTEGER",
            "BIGINT",
            "SMALLINT",
            "FLOAT",
            "DOUBLE",
            "DECIMAL",
            "NUMERIC",
            "REAL",
            "CHAR",
            "VARCHAR",
            "TEXT",
            "BOOLEAN",
            "SERIAL",
            "BIGSERIAL",
            "JSON",
            "JSONB",
            "UUID",
            "BYTEA",
            "ARRAY",
            "ILIKE",
            "SIMILAR",
            "ANY",
            "SOME",
            "FETCH",
            "FIRST",
            "NEXT",
            "ROWS",
            "ONLY",
            "LATERAL",
            "GENERATED",
            "ALWAYS",
            "IDENTITY",
            "STORED",
            "VIRTUAL",
        };
        return keywords;
    }

    void TextEditor::SetPalette(const Palette& palette) {
        // callers set it every frame to follow the theme: only a real change rehighlights
        if (palette == palette_)
            return;
        palette_ = palette;
        tsPreviousColors_.clear(); // every color changes
        highlightDirty_ = true;
    }

    void TextEditor::SetShowLineNumbers(bool show) {
        showLineNumbers_ = show;
    }
    void TextEditor::SetTabSize(int size) {
        tabSize_ = size;
    }
    void TextEditor::SetReadOnly(bool readOnly) {
        readOnly_ = readOnly;
    }

    TextEditor::Palette TextEditor::GetDarkPalette() {
        Palette p{};
        p.background = IM_COL32(30, 30, 46, 255);
        p.text = IM_COL32(205, 214, 244, 255);
        p.keyword = IM_COL32(203, 166, 247, 255);
        p.string = IM_COL32(166, 227, 161, 255);
        p.number = IM_COL32(250, 179, 135, 255);
        p.comment = IM_COL32(108, 112, 134, 255);
        p.function = IM_COL32(137, 180, 250, 255);
        p.type = IM_COL32(249, 226, 175, 255);
        p.operator_ = IM_COL32(148, 226, 213, 255);
        p.cursor = IM_COL32(205, 214, 244, 255);
        p.selection = IM_COL32(88, 91, 112, 120);
        p.lineNumber = IM_COL32(88, 91, 112, 255);
        p.currentLineNumber = IM_COL32(205, 214, 244, 255);
        p.currentLineBackground = IM_COL32(49, 50, 68, 255);
        p.lineNumberBackground = IM_COL32(30, 30, 46, 255);
        return p;
    }

    TextEditor::Palette TextEditor::GetLightPalette() {
        Palette p{};
        p.background = IM_COL32(239, 241, 245, 255);
        p.text = IM_COL32(76, 79, 105, 255);
        p.keyword = IM_COL32(136, 57, 239, 255);
        p.string = IM_COL32(64, 160, 43, 255);
        p.number = IM_COL32(254, 100, 11, 255);
        p.comment = IM_COL32(156, 160, 176, 255);
        p.function = IM_COL32(30, 102, 245, 255);
        p.type = IM_COL32(223, 142, 29, 255);
        p.operator_ = IM_COL32(23, 146, 153, 255);
        p.cursor = IM_COL32(76, 79, 105, 255);
        p.selection = IM_COL32(172, 176, 190, 120);
        p.lineNumber = IM_COL32(156, 160, 176, 255);
        p.currentLineNumber = IM_COL32(76, 79, 105, 255);
        p.currentLineBackground = IM_COL32(230, 233, 239, 255);
        p.lineNumberBackground = IM_COL32(239, 241, 245, 255);
        return p;
    }

    static ImU32 toImU32(const ImVec4& c) {
        return IM_COL32(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255),
                        static_cast<int>(c.z * 255), static_cast<int>(c.w * 255));
    }

    static ImU32 toImU32Alpha(const ImVec4& c, int alpha) {
        return IM_COL32(static_cast<int>(c.x * 255), static_cast<int>(c.y * 255),
                        static_cast<int>(c.z * 255), alpha);
    }

    TextEditor::Palette TextEditor::FromTheme(const Theme::Colors& colors) {
        Palette p{};
        p.background = toImU32(colors.base);
        p.text = toImU32(colors.text);
        p.keyword = toImU32(colors.purple);
        p.string = toImU32(colors.green);
        p.number = toImU32(colors.peach);
        p.comment = toImU32(colors.overlay2);
        p.function = toImU32(colors.blue);
        p.type = toImU32(colors.yellow);
        p.operator_ = toImU32(colors.teal);
        p.cursor = toImU32(colors.text);
        p.selection =
            toImU32Alpha(colors.blue, 90); // accent: a grey surface vanishes on the background
        p.lineNumber = toImU32(colors.overlay2);
        p.currentLineNumber = toImU32(colors.text);
        p.currentLineBackground = toImU32(colors.surface0);
        p.lineNumberBackground = toImU32(colors.base);
        p.run = toImU32(colors.green);
        p.error = toImU32(colors.red);
        return p;
    }

    // --- Content Management ---

    void TextEditor::rebuildLineStarts() {
        lineStarts_.clear();
        lineStarts_.push_back(0);
        for (size_t i = 0; i < content_.size(); ++i) {
            if (content_[i] == '\n')
                lineStarts_.push_back(static_cast<int>(i + 1));
        }
        contentWidthDirty_ = true;
        contentDirty_ = true;
    }

    int TextEditor::getLineFromPos(int pos) const {
        auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), pos);
        return static_cast<int>(std::distance(lineStarts_.begin(), it)) - 1;
    }

    int TextEditor::getColumnFromPos(int pos) const {
        int line = getLineFromPos(pos);
        return pos - lineStarts_[line];
    }

    int TextEditor::getPosFromLineColumn(int line, int column) const {
        if (line < 0)
            return 0;
        if (line >= static_cast<int>(lineStarts_.size()))
            return static_cast<int>(content_.size());
        int lineStart = lineStarts_[line];
        int lineEnd = getLineEnd(line);
        return std::min(lineStart + column, lineEnd);
    }

    int TextEditor::getLineEnd(int line) const {
        if (line + 1 < static_cast<int>(lineStarts_.size()))
            return lineStarts_[line + 1] - 1; // before the \n
        return static_cast<int>(content_.size());
    }

    // --- Rendering ---

    void TextEditor::Render(const char* label, ImVec2 size, bool border) {
        ImGui::PushID(label);

        // Compute layout
        lineHeight_ = ImGui::GetTextLineHeightWithSpacing();

        // Line number width
        if (showLineNumbers_) {
            int maxLine = static_cast<int>(lineStarts_.size());
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", readOnly_ ? maxLine : std::max(maxLine, 999));
            lineNumberWidth_ = ImGui::CalcTextSize(buf).x + 14.0f;
            if (rangeProvider_) {
                lineNumberWidth_ += ImGui::GetFontSize() + 6.0f; // the play icons
            }
        } else {
            lineNumberWidth_ = 0.0f;
        }

        // every edit marks the highlight dirty: the statements may have moved too
        if (rangeProvider_ && (highlightDirty_ || runRangesDirty_)) {
            // big scripts (dumps): no icons while typing, one split after a pause, so a
            // stale range is never clickable
            const double now = ImGui::GetTime();
            if (highlightDirty_)
                runRangesEditTime_ = now;
            if (content_.size() < 256 * 1024 || now - runRangesEditTime_ > 0.3) {
                runRanges_ = rangeProvider_(content_);
                runRangesDirty_ = false;
            } else {
                runRanges_.clear();
                runRangesDirty_ = true;
            }
        }

        // Rehighlight if needed
        if (highlightDirty_) {
            rehighlight();
            highlightDirty_ = false;
        }

        // Ensure colors match content size
        if (colors_.size() != content_.size()) {
            colors_.resize(content_.size(), palette_.text);
        }

        // Begin editor child window
        ImGui::PushStyleColor(ImGuiCol_ChildBg, palette_.background);
        ImGui::BeginChild("##editor_outer", size, border ? ImGuiChildFlags_Borders : 0);

        // Handle focus
        if (focusRequested_) {
            ImGui::SetWindowFocus();
            focusRequested_ = false;
        }

        // Clicking anywhere in the editor should focus it
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImGui::SetWindowFocus();
        }

        // this editor only: several can share a root window (code blocks in a chat)
        bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

        // Don't handle input if an ImGui text widget (InputText, etc.) is active,
        // since it reads from the same io.InputQueueCharacters queue.
        bool imguiWantsText = ImGui::GetIO().WantTextInput;

        // Cursor blink
        cursorBlinkTimer_ += ImGui::GetIO().DeltaTime;

        // Input handling
        if (isFocused && !imguiWantsText) { // read-only still copies and moves the cursor
            handleKeyboardInput();
        }
#ifdef DEARSQL_MOBILE
        // asks for the soft keyboard the way imgui text fields do, minus WantTextInput, which
        // would switch off the input handling above
        if (isFocused && !readOnly_)
            ImGui::GetCurrentContext()->PlatformImeData.WantVisible = true;
#endif
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            handleMouseInput();
        }

        // --- Line numbers panel ---
        ImVec2 lineNumbersScreenPos = ImGui::GetCursorScreenPos();
        if (showLineNumbers_) {
            // space only: numbers and play icons are drawn after the text area, which
            // knows the scroll. not a child window, which would sit over the icons
            ImGui::Dummy(ImVec2(lineNumberWidth_, ImGui::GetContentRegionAvail().y));
            ImGui::SameLine(0, 0);
        }

        // --- Text content area ---
        float textAreaWidth = ImGui::GetContentRegionAvail().x;
        float spaceWidth = ImGui::CalcTextSize(" ").x;

        // recompute max content width only when content changed
        if (contentWidthDirty_) {
            float maxW = 0.0f;
            for (size_t i = 0; i < lineStarts_.size(); ++i) {
                int start = lineStarts_[i];
                int end = getLineEnd(static_cast<int>(i));
                if (end > start) {
                    float w = 0;
                    for (int j = start; j < end; ++j) {
                        if (content_[j] == '\t')
                            w += spaceWidth * tabSize_;
                        else
                            w += ImGui::CalcTextSize(&content_[j], &content_[j] + 1).x;
                    }
                    maxW = std::max(maxW, w);
                }
            }
            cachedContentWidth_ = maxW + spaceWidth * 10;
            contentWidthDirty_ = false;
        }
        float contentWidth = cachedContentWidth_;
        // read-only: no spare line to type into
        float contentHeight = lineStarts_.size() * lineHeight_ + (readOnly_ ? 4.0f : lineHeight_);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::SetNextWindowContentSize(ImVec2(contentWidth, contentHeight));
        ImGui::BeginChild("##text_area", ImVec2(textAreaWidth, -1), false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

        scrollX_ = ImGui::GetScrollX();
        scrollY_ = ImGui::GetScrollY();
        textAreaMin_ = ImGui::GetWindowPos();
        textAreaMax_ = ImVec2(textAreaMin_.x + ImGui::GetWindowWidth(),
                              textAreaMin_.y + ImGui::GetWindowHeight());

        textOrigin_ = ImGui::GetCursorScreenPos();
        textOrigin_.x += 2.0f; // left margin: the current-line highlight starts at the text
        textOrigin_.y += 2.0f; // top margin

        // Compute visible line range
        float windowHeight = ImGui::GetWindowHeight();
        int startLine = std::max(0, static_cast<int>(scrollY_ / lineHeight_) - 2);
        int endLine = std::min(static_cast<int>(lineStarts_.size()) - 1,
                               static_cast<int>((scrollY_ + windowHeight) / lineHeight_) + 2);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Render layers
        if (!readOnly_)
            renderCurrentLineHighlight(drawList);
        renderFindHighlights(drawList, startLine, endLine);
        renderSelection(drawList, startLine, endLine);
        renderText(drawList, startLine, endLine);
        // outline the statement whose play icon is hovered (last frame: the icons are
        // drawn after this child), on top of the current-line highlight
        if (hoveredRun_ >= 0 && hoveredRun_ < static_cast<int>(runRanges_.size())) {
            const auto [begin, end] = runRanges_[hoveredRun_];
            const float top = textOrigin_.y + getLineFromPos(static_cast<int>(begin)) * lineHeight_;
            const float bottom =
                textOrigin_.y + (getLineFromPos(static_cast<int>(end)) + 1) * lineHeight_;
            drawList->AddRect(ImVec2(textAreaMin_.x + 1.0f, top),
                              ImVec2(textAreaMax_.x - 1.0f, bottom), palette_.run, 3.0f, 0, 1.5f);
        }
        renderErrorMarker(drawList);
        if (content_.empty() && !placeholder_.empty()) {
            const ImU32 placeholderColor = (palette_.text & 0x00FFFFFF) | 0x60000000;
            const float x = textOrigin_.x;
            float y = textOrigin_.y;
            const char* start = placeholder_.c_str();
            const char* end = start + placeholder_.size();
            while (start < end) {
                const char* lineEnd = static_cast<const char*>(memchr(start, '\n', end - start));
                if (!lineEnd)
                    lineEnd = end;
                drawList->AddText(ImVec2(x, y), placeholderColor, start, lineEnd);
                y += lineHeight_;
                start = lineEnd < end ? lineEnd + 1 : end;
            }
        }
        if (isFocused && !readOnly_)
            renderCursor(drawList);

        // Ensure cursor visible
        ensureCursorVisible();

        // Extend content area
        ImGui::SetCursorPosY(contentHeight);
        ImGui::Dummy(ImVec2(0, 0));

        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Render line numbers (after text area so we have scroll position)
        if (showLineNumbers_) {
            renderLineNumbers(ImGui::GetWindowDrawList(), startLine, endLine);
            renderRunButtons(startLine, endLine);
        }

        // Render autocomplete popup
        if (autocompleteVisible_)
            renderAutoComplete();

        // Find/replace bar
        renderFindReplace();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // --- Rendering helpers ---

    void TextEditor::renderLineNumbers(ImDrawList* drawList, int startLine, int endLine) {
        char buf[16];
        int currentLine = getLineFromPos(cursorIndex_);

        for (int i = startLine; i <= endLine; ++i) {
            // textOrigin_.y already accounts for scroll (captured inside the
            // scrolled text_area child), so don't subtract scrollY_ here.
            float y = textOrigin_.y + i * lineHeight_;
            snprintf(buf, sizeof(buf), "%d", i + 1);

            float textW = ImGui::CalcTextSize(buf).x;
            float x = textOrigin_.x - 10.0f - textW;

            ImU32 color =
                (i == currentLine && !readOnly_) ? palette_.currentLineNumber : palette_.lineNumber;
            drawList->AddText(ImVec2(x, y), color, buf);
        }
    }

    void TextEditor::SetErrorMarker(size_t offset, std::string message) {
        errorOffset_ = static_cast<int>(std::min(offset, content_.size()));
        errorMessage_ = std::move(message);
    }

    void TextEditor::renderErrorMarker(ImDrawList* drawList) {
        if (errorOffset_ < 0 || errorOffset_ > static_cast<int>(content_.size())) {
            return;
        }
        auto word = [&](int i) {
            const auto c = static_cast<unsigned char>(content_[i]);
            return std::isalnum(c) || c == '_' || c >= 0x80;
        };
        auto blank = [&](int i) { return std::isspace(static_cast<unsigned char>(content_[i])); };
        const int size = static_cast<int>(content_.size());
        int b = errorOffset_;
        // at the end or on a gap ('unexpected end'): mark the token before it
        while (b > 0 && (b == size || blank(b))) {
            --b;
        }
        if (b >= size || blank(b)) {
            return;
        }
        int e = b + 1;
        if (word(b)) {
            while (b > 0 && word(b - 1)) {
                --b;
            }
            while (e < size && word(e)) {
                ++e;
            }
        }
        const int line = getLineFromPos(b);
        const float x0 = getScreenX(b);
        const float x1 = std::max(getScreenX(e), x0 + 6.0f);
        const float top = textOrigin_.y + line * lineHeight_;
        const float y = top + lineHeight_; // just under the glyphs
        // a zigzag, as editors draw it
        constexpr float step = 2.5f;
        ImVec2 points[256];
        int n = 0;
        for (float x = x0; x < x1 && n < 255; x += step, ++n) {
            points[n] = ImVec2(x, y + ((n & 1) ? -1.2f : 0.8f));
        }
        points[n++] = ImVec2(x1, y);
        drawList->AddPolyline(points, n, palette_.error, ImDrawFlags_None, 1.2f);
        if (ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(ImVec2(x0, top), ImVec2(x1, top + lineHeight_))) {
            ImGui::SetTooltip("%s", errorMessage_.c_str());
        }
    }

    void TextEditor::renderRunButtons(int startLine, int endLine) {
        hoveredRun_ = -1;
        if (!rangeProvider_) {
            return;
        }
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float iconX = textAreaMin_.x - lineNumberWidth_ + 6.0f; // the gutter's left edge
        const float iconW = ImGui::GetFontSize();
        for (size_t r = 0; r < runRanges_.size(); ++r) {
            const auto [begin, end] = runRanges_[r];
            const int line = getLineFromPos(static_cast<int>(begin));
            if (line < startLine || line > endLine) {
                continue;
            }
            const float y = textOrigin_.y + line * lineHeight_;
            const ImVec2 hitMin(iconX - 3.0f, y);
            const ImVec2 hitMax(iconX + iconW + 3.0f, y + lineHeight_);
            // a real item, so tests can click it by id (##run_<n>)
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(hitMin);
            const bool clicked = ImGui::InvisibleButton(std::format("##run_{}", r).c_str(),
                                                        ImVec2(hitMax.x - hitMin.x, lineHeight_));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(cursor);
            // dimmed until hovered, so a long script is not a column of bright icons
            const ImU32 color = hovered ? palette_.run : (palette_.run & 0x00FFFFFF) | 0x99000000;
            drawList->AddText(ImVec2(iconX, y), color, ICON_FA_PLAY);
            if (!hovered) {
                continue;
            }
            hoveredRun_ = static_cast<int>(r);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (clicked && onRun_) {
                onRun_(begin, end);
            }
        }
    }

    void TextEditor::renderText(ImDrawList* drawList, int startLine, int endLine) {
        float spaceWidth = ImGui::CalcTextSize(" ").x;

        for (int lineNum = startLine; lineNum <= endLine; ++lineNum) {
            int lineStart = lineStarts_[lineNum];
            int lineEnd = getLineEnd(lineNum);

            float xPos = textOrigin_.x;
            float yPos = textOrigin_.y + lineNum * lineHeight_;

            for (int i = lineStart; i < lineEnd; ++i) {
                char c = content_[i];
                if (c == '\n')
                    break;

                if (c == '\t') {
                    int col = static_cast<int>((xPos - textOrigin_.x) / spaceWidth);
                    int nextTab = ((col / tabSize_) + 1) * tabSize_;
                    xPos = textOrigin_.x + nextTab * spaceWidth;
                    continue;
                }

                // Handle UTF-8 multi-byte
                const char* charStart = &content_[i];
                const char* charEnd = charStart + 1;
                if (static_cast<unsigned char>(*charStart) >= 0x80) {
                    while (charEnd < content_.data() + content_.size() &&
                           (static_cast<unsigned char>(*charEnd) & 0xC0) == 0x80)
                        ++charEnd;
                }

                ImU32 color = (i < static_cast<int>(colors_.size())) ? colors_[i] : palette_.text;
                float charW = ImGui::CalcTextSize(charStart, charEnd).x;

                drawList->AddText(ImVec2(xPos, yPos), color, charStart, charEnd);
                xPos += charW;

                // Skip continuation bytes
                int bytesInChar = static_cast<int>(charEnd - charStart);
                if (bytesInChar > 1)
                    i += bytesInChar - 1;
            }
        }
    }

    void TextEditor::renderCursor(ImDrawList* drawList) {
        float blinkAlpha = (sinf(cursorBlinkTimer_ * 5.0f) + 1.0f) * 0.5f;
        if (blinkAlpha < 0.3f)
            return;

        int line = getLineFromPos(cursorIndex_);
        float cursorX = getCursorScreenX();
        float cursorY = textOrigin_.y + line * lineHeight_;

        ImU32 color = palette_.cursor;
        // Apply blink alpha
        int a = static_cast<int>(((color >> 24) & 0xFF) * blinkAlpha);
        color = (color & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24);

        drawList->AddLine(ImVec2(cursorX, cursorY), ImVec2(cursorX, cursorY + lineHeight_ - 1),
                          color, 2.0f);
    }

    void TextEditor::renderSelection(ImDrawList* drawList, int startLine, int endLine) {
        if (!selectionActive_)
            return;

        int selStart = selectionStart();
        int selEnd = selectionEnd();
        if (selStart == selEnd)
            return;

        float spaceWidth = ImGui::CalcTextSize(" ").x;

        for (int lineNum = startLine; lineNum <= endLine; ++lineNum) {
            int lineStart = lineStarts_[lineNum];
            int lineEnd = (lineNum + 1 < static_cast<int>(lineStarts_.size()))
                              ? lineStarts_[lineNum + 1]
                              : static_cast<int>(content_.size());

            int rangeStart = std::max(selStart, lineStart);
            int rangeEnd = std::min(selEnd, lineEnd);
            if (rangeStart >= rangeEnd)
                continue;

            // Calculate x positions for range
            auto calcXForPos = [&](int pos) -> float {
                float x = textOrigin_.x;
                for (int i = lineStart; i < pos && i < lineEnd; ++i) {
                    if (content_[i] == '\t') {
                        int col = static_cast<int>((x - textOrigin_.x) / spaceWidth);
                        int nextTab = ((col / tabSize_) + 1) * tabSize_;
                        x = textOrigin_.x + nextTab * spaceWidth;
                    } else if (content_[i] != '\n') {
                        const char* cs = &content_[i];
                        const char* ce = cs + 1;
                        if (static_cast<unsigned char>(*cs) >= 0x80) {
                            while (ce < content_.data() + content_.size() &&
                                   (static_cast<unsigned char>(*ce) & 0xC0) == 0x80)
                                ++ce;
                        }
                        x += ImGui::CalcTextSize(cs, ce).x;
                        int bytes = static_cast<int>(ce - cs);
                        if (bytes > 1)
                            i += bytes - 1;
                    }
                }
                return x;
            };

            float x1 = calcXForPos(rangeStart);
            float x2 = calcXForPos(rangeEnd);
            if (content_[rangeEnd - 1] == '\n')
                x2 += spaceWidth; // extend selection past newline

            float y = textOrigin_.y + lineNum * lineHeight_;
            drawList->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + lineHeight_), palette_.selection);
        }
    }

    void TextEditor::renderCurrentLineHighlight(ImDrawList* drawList) {
        int currentLine = getLineFromPos(cursorIndex_);
        // Don't highlight if there's an active selection spanning multiple lines
        if (selectionActive_ && selectionStart() != selectionEnd()) {
            int selStartLine = getLineFromPos(selectionStart());
            int selEndLine = getLineFromPos(selectionEnd());
            if (selStartLine != selEndLine)
                return;
        }

        float y = textOrigin_.y + currentLine * lineHeight_;
        ImVec2 winPos = ImGui::GetWindowPos();
        float winWidth = ImGui::GetWindowWidth();
        drawList->AddRectFilled(ImVec2(winPos.x, y), ImVec2(winPos.x + winWidth, y + lineHeight_),
                                palette_.currentLineBackground);
    }

    // --- Cursor helpers ---

    float TextEditor::getScreenX(int pos) const {
        int line = getLineFromPos(pos);
        int lineStart = lineStarts_[line];
        float spaceWidth = ImGui::CalcTextSize(" ").x;
        float x = textOrigin_.x;

        for (int i = lineStart; i < pos && i < static_cast<int>(content_.size()); ++i) {
            if (content_[i] == '\t') {
                int col = static_cast<int>((x - textOrigin_.x) / spaceWidth);
                int nextTab = ((col / tabSize_) + 1) * tabSize_;
                x = textOrigin_.x + nextTab * spaceWidth;
            } else if (content_[i] == '\n') {
                break;
            } else {
                const char* cs = &content_[i];
                const char* ce = cs + 1;
                if (static_cast<unsigned char>(*cs) >= 0x80) {
                    while (ce < content_.data() + content_.size() &&
                           (static_cast<unsigned char>(*ce) & 0xC0) == 0x80)
                        ++ce;
                }
                x += ImGui::CalcTextSize(cs, ce).x;
                int bytes = static_cast<int>(ce - cs);
                if (bytes > 1)
                    i += bytes - 1;
            }
        }
        return x;
    }

    void TextEditor::ensureCursorVisible() {
        if (!ensureCursorVisibleV_ && !ensureCursorVisibleH_)
            return;

        int line = getLineFromPos(cursorIndex_);
        float cursorY = line * lineHeight_;
        float windowHeight = ImGui::GetWindowHeight();

        if (ensureCursorVisibleV_) {
            if (cursorY < scrollY_)
                ImGui::SetScrollY(cursorY);
            else if (cursorY + lineHeight_ > scrollY_ + windowHeight)
                ImGui::SetScrollY(cursorY + lineHeight_ - windowHeight);
            ensureCursorVisibleV_ = false;
        }

        if (ensureCursorVisibleH_) {
            float cursorX = getCursorScreenX() - textOrigin_.x;
            float windowWidth = ImGui::GetWindowWidth();
            if (cursorX < scrollX_)
                ImGui::SetScrollX(std::max(0.0f, cursorX - 20.0f));
            else if (cursorX > scrollX_ + windowWidth - 20.0f)
                ImGui::SetScrollX(cursorX - windowWidth + 40.0f);
            ensureCursorVisibleH_ = false;
        }
    }

    // --- Selection helpers ---

    int TextEditor::selectionStart() const {
        return std::min(cursorIndex_, selectionAnchor_);
    }

    int TextEditor::selectionEnd() const {
        return std::max(cursorIndex_, selectionAnchor_);
    }

    void TextEditor::deleteSelection() {
        if (!selectionActive_)
            return;
        int start = selectionStart();
        int end = selectionEnd();
        if (start == end)
            return;

        content_.erase(start, end - start);
        colors_.erase(colors_.begin() + start,
                      colors_.begin() + std::min(end, static_cast<int>(colors_.size())));
        cursorIndex_ = start;
        clearSelection();
        rebuildLineStarts();
        highlightDirty_ = true;
    }

    std::string TextEditor::getSelectedText() const {
        if (!selectionActive_)
            return "";
        int start = selectionStart();
        int end = selectionEnd();
        if (start == end)
            return "";
        return content_.substr(start, end - start);
    }

    void TextEditor::clearSelection() {
        selectionActive_ = false;
        selectionAnchor_ = cursorIndex_;
    }

    // --- Word helpers ---

    bool TextEditor::isWordChar(char c) const {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    int TextEditor::findWordStart(int pos) const {
        if (pos <= 0)
            return 0;
        --pos;
        while (pos > 0 && isWordChar(content_[pos]))
            --pos;
        if (!isWordChar(content_[pos]))
            ++pos;
        return pos;
    }

    int TextEditor::findWordEnd(int pos) const {
        int sz = static_cast<int>(content_.size());
        while (pos < sz && isWordChar(content_[pos]))
            ++pos;
        return pos;
    }

    void TextEditor::insertTextAtCursor(const std::string& text) {
        if (selectionActive_)
            deleteSelection();

        int pos = std::clamp(cursorIndex_, 0, static_cast<int>(content_.size()));
        content_.insert(pos, text);

        // Insert colors for new text
        ImU32 defColor = palette_.text;
        if (pos > 0 && pos <= static_cast<int>(colors_.size()))
            defColor = colors_[pos - 1];
        colors_.insert(colors_.begin() + pos, text.size(), defColor);

        cursorIndex_ = pos + static_cast<int>(text.size());
        selectionAnchor_ = cursorIndex_;
        rebuildLineStarts();
        highlightDirty_ = true;
    }

    void TextEditor::pushUndoSnapshot() {
        if (content_ == lastSnapshotContent_)
            return;
        undoStack_.push_back({lastSnapshotContent_, cursorIndex_});
        if (undoStack_.size() > 100)
            undoStack_.erase(undoStack_.begin());
        redoStack_.clear();
        lastSnapshotContent_ = content_;
    }

    float TextEditor::getCharWidth(const char* start, const char* end) const {
        return ImGui::CalcTextSize(start, end).x;
    }

    std::string TextEditor::getCurrentWord() const {
        int start = cursorIndex_;
        while (start > 0 && isWordChar(content_[start - 1]))
            --start;
        if (start == cursorIndex_)
            return "";
        return content_.substr(start, cursorIndex_ - start);
    }

} // namespace dearsql
