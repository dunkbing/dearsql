#include "ui/text_editor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

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
          completionItems_(std::move(other.completionItems_)), tsParser_(other.tsParser_),
          tsTree_(other.tsTree_), tsQuery_(other.tsQuery_),
          tsPreviousContent_(std::move(other.tsPreviousContent_)),
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
            tsParser_ = other.tsParser_;
            tsTree_ = other.tsTree_;
            tsQuery_ = other.tsQuery_;
            tsPreviousContent_ = std::move(other.tsPreviousContent_);
            highlightDirty_ = other.highlightDirty_;
            other.tsParser_ = nullptr;
            other.tsTree_ = nullptr;
            other.tsQuery_ = nullptr;
        }
        return *this;
    }

    void TextEditor::SetText(const std::string& text) {
        content_ = text;
        cursorIndex_ = static_cast<int>(text.size());
        selectionActive_ = false;
        selectionAnchor_ = cursorIndex_;
        rebuildLineStarts();
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
    }

    void TextEditor::SetCompletionKeywords(const std::vector<std::string>& keywords) {
        completionItems_.clear();
        completionItems_.reserve(keywords.size());
        for (const auto& kw : keywords)
            completionItems_.push_back({kw, CompletionKind::Keyword});
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
        palette_ = palette;
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
        p.keyword = toImU32(colors.mauve);
        p.string = toImU32(colors.green);
        p.number = toImU32(colors.peach);
        p.comment = toImU32(colors.overlay2);
        p.function = toImU32(colors.blue);
        p.type = toImU32(colors.yellow);
        p.operator_ = toImU32(colors.teal);
        p.cursor = toImU32(colors.text);
        p.selection = toImU32Alpha(colors.surface2, 120);
        p.lineNumber = toImU32(colors.overlay2);
        p.currentLineNumber = toImU32(colors.text);
        p.currentLineBackground = toImU32(colors.surface0);
        p.lineNumberBackground = toImU32(colors.base);
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
            snprintf(buf, sizeof(buf), "%d", std::max(maxLine, 999));
            lineNumberWidth_ = ImGui::CalcTextSize(buf).x + 20.0f;
        } else {
            lineNumberWidth_ = 0.0f;
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

        bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        // Don't handle input if an ImGui text widget (InputText, etc.) is active,
        // since it reads from the same io.InputQueueCharacters queue.
        bool imguiWantsText = ImGui::GetIO().WantTextInput;

        // Cursor blink
        cursorBlinkTimer_ += ImGui::GetIO().DeltaTime;

        // Input handling
        if (isFocused && !readOnly_ && !imguiWantsText) {
            handleKeyboardInput();
        }
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            handleMouseInput();
        }

        // --- Line numbers panel ---
        ImVec2 lineNumbersScreenPos = ImGui::GetCursorScreenPos();
        if (showLineNumbers_) {
            ImGui::BeginChild("##line_nums", ImVec2(lineNumberWidth_, -1), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::EndChild();
            ImGui::SameLine(0, 0);
        }

        // --- Text content area ---
        float textAreaWidth = ImGui::GetContentRegionAvail().x;
        float contentWidth = 0.0f;

        // Estimate max content width
        ImFont* font = ImGui::GetFont();
        float spaceWidth = ImGui::CalcTextSize(" ").x;
        for (size_t i = 0; i < lineStarts_.size(); ++i) {
            int start = lineStarts_[i];
            int end = getLineEnd(i);
            if (end > start) {
                float w = 0;
                for (int j = start; j < end; ++j) {
                    if (content_[j] == '\t')
                        w += spaceWidth * tabSize_;
                    else
                        w += ImGui::CalcTextSize(&content_[j], &content_[j] + 1).x;
                }
                contentWidth = std::max(contentWidth, w);
            }
        }
        contentWidth += spaceWidth * 10;
        float contentHeight = lineStarts_.size() * lineHeight_ + lineHeight_;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::SetNextWindowContentSize(ImVec2(contentWidth, contentHeight));
        ImGui::BeginChild("##text_area", ImVec2(textAreaWidth, -1), false,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

        scrollX_ = ImGui::GetScrollX();
        scrollY_ = ImGui::GetScrollY();

        textOrigin_ = ImGui::GetCursorScreenPos();
        textOrigin_.x += 8.0f; // left margin
        textOrigin_.y += 2.0f; // top margin

        // Compute visible line range
        float windowHeight = ImGui::GetWindowHeight();
        int startLine = std::max(0, static_cast<int>(scrollY_ / lineHeight_) - 2);
        int endLine = std::min(static_cast<int>(lineStarts_.size()) - 1,
                               static_cast<int>((scrollY_ + windowHeight) / lineHeight_) + 2);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Render layers
        renderCurrentLineHighlight(drawList);
        renderFindHighlights(drawList, startLine, endLine);
        renderSelection(drawList, startLine, endLine);
        renderText(drawList, startLine, endLine);
        if (isFocused)
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
            float y = textOrigin_.y + i * lineHeight_ - scrollY_;
            snprintf(buf, sizeof(buf), "%d", i + 1);

            float textW = ImGui::CalcTextSize(buf).x;
            float x = textOrigin_.x - 8.0f - lineNumberWidth_ + lineNumberWidth_ - textW - 10.0f;

            ImU32 color = (i == currentLine) ? palette_.currentLineNumber : palette_.lineNumber;
            drawList->AddText(ImVec2(x, y), color, buf);
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
            if (rangeEnd > rangeStart && content_[rangeEnd - 1] == '\n')
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

    float TextEditor::getCursorScreenX() const {
        int line = getLineFromPos(cursorIndex_);
        int lineStart = lineStarts_[line];
        float spaceWidth = ImGui::CalcTextSize(" ").x;
        float x = textOrigin_.x;

        for (int i = lineStart; i < cursorIndex_ && i < static_cast<int>(content_.size()); ++i) {
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
