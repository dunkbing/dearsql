#include "ui/text_editor.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace dearsql {
    namespace {
        constexpr int kAutocompleteMaxVisible = 8;
    }

    // --- Keyboard Input ---

    void TextEditor::handleKeyboardInput() {
        ImGuiIO& io = ImGui::GetIO();
        bool ctrl = io.KeyCtrl;
        bool shift = io.KeyShift;

        // Autocomplete navigation takes priority
        if (autocompleteVisible_) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                const int count = static_cast<int>(filteredCompletions_.size());
                if (autocompleteIndex_ >= count - 1) {
                    autocompleteIndex_ = 0;
                    autocompleteScrollOffset_ = 0;
                } else {
                    autocompleteIndex_++;
                    if (autocompleteIndex_ >= autocompleteScrollOffset_ + kAutocompleteMaxVisible)
                        autocompleteScrollOffset_ =
                            autocompleteIndex_ - kAutocompleteMaxVisible + 1;
                }
                return;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                const int count = static_cast<int>(filteredCompletions_.size());
                if (autocompleteIndex_ <= 0) {
                    autocompleteIndex_ = count - 1;
                    autocompleteScrollOffset_ = std::max(0, count - kAutocompleteMaxVisible);
                } else {
                    autocompleteIndex_--;
                    if (autocompleteIndex_ < autocompleteScrollOffset_)
                        autocompleteScrollOffset_ = autocompleteIndex_;
                }
                return;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                applyAutoComplete();
                return;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                dismissAutoComplete();
                return;
            }
        }

        // Escape closes find bar
        if (findVisible_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            closeFind();
            return;
        }

        if (ctrl) {
            // Ctrl+Enter submits
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                if (submitCallback_)
                    submitCallback_();
                return;
            }
            // Ctrl+F opens find
            if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                openFind();
                return;
            }
            // Ctrl+H opens find+replace
            if (ImGui::IsKeyPressed(ImGuiKey_H)) {
                openFind();
                return;
            }
            // Ctrl+/ toggles line comment
            if (ImGui::IsKeyPressed(ImGuiKey_Slash)) {
                toggleLineComment();
                return;
            }
            handleClipboard();
            handleUndoRedo();
            handleSelectAll();
        }

        handleArrowKeys();
        handleHomeEnd();

        if (!readOnly_) {
            handleCharacterInput();
            handleEnterKey();
            handleBackspaceKey();
            handleDeleteKey();
            handleTabKey();
        }
    }

    void TextEditor::handleCharacterInput() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl || io.KeySuper)
            return;

        std::string input;
        for (int n = 0; n < io.InputQueueCharacters.Size; ++n) {
            ImWchar ch = io.InputQueueCharacters[n];
            if (ch >= 32) {
                // Convert ImWchar to UTF-8
                char buf[5] = {};
                if (ch < 0x80) {
                    buf[0] = static_cast<char>(ch);
                } else if (ch < 0x800) {
                    buf[0] = static_cast<char>(0xC0 | (ch >> 6));
                    buf[1] = static_cast<char>(0x80 | (ch & 0x3F));
                } else {
                    buf[0] = static_cast<char>(0xE0 | (ch >> 12));
                    buf[1] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                    buf[2] = static_cast<char>(0x80 | (ch & 0x3F));
                }
                input += buf;
            }
        }

        if (input.empty())
            return;

        pushUndoSnapshot();
        insertTextAtCursor(input);
        cursorBlinkTimer_ = 0.0f;
        ensureCursorVisibleV_ = true;
        ensureCursorVisibleH_ = true;
        updateAutoComplete();
    }

    void TextEditor::handleEnterKey() {
        if (!ImGui::IsKeyPressed(ImGuiKey_Enter))
            return;
        if (autocompleteVisible_)
            return; // handled above

        pushUndoSnapshot();

        if (selectionActive_)
            deleteSelection();

        // Calculate indentation from current line
        int line = getLineFromPos(cursorIndex_);
        int lineStart = lineStarts_[line];
        std::string indent;
        for (int i = lineStart; i < static_cast<int>(content_.size()); ++i) {
            char c = content_[i];
            if (c == ' ' || c == '\t')
                indent += c;
            else
                break;
        }

        // Limit indent to cursor position within the line
        int cursorCol = cursorIndex_ - lineStart;
        if (static_cast<int>(indent.size()) > cursorCol)
            indent = indent.substr(0, cursorCol);

        std::string toInsert = "\n" + indent;
        content_.insert(cursorIndex_, toInsert);
        colors_.insert(colors_.begin() + cursorIndex_, toInsert.size(), palette_.text);
        cursorIndex_ += static_cast<int>(toInsert.size());
        selectionAnchor_ = cursorIndex_;

        rebuildLineStarts();
        highlightDirty_ = true;
        cursorBlinkTimer_ = 0.0f;
        ensureCursorVisibleV_ = true;
        ensureCursorVisibleH_ = true;
        dismissAutoComplete();
    }

    void TextEditor::handleBackspaceKey() {
        if (!ImGui::IsKeyPressed(ImGuiKey_Backspace))
            return;

        pushUndoSnapshot();

        if (selectionActive_ && selectionStart() != selectionEnd()) {
            deleteSelection();
        } else if (cursorIndex_ > 0) {
            // Find start of previous character (UTF-8 aware)
            int prevPos = cursorIndex_ - 1;
            while (prevPos > 0 && (static_cast<unsigned char>(content_[prevPos]) & 0xC0) == 0x80)
                --prevPos;

            int deleteLen = cursorIndex_ - prevPos;
            content_.erase(prevPos, deleteLen);
            if (prevPos < static_cast<int>(colors_.size()))
                colors_.erase(colors_.begin() + prevPos,
                              colors_.begin() +
                                  std::min(cursorIndex_, static_cast<int>(colors_.size())));
            cursorIndex_ = prevPos;
            selectionAnchor_ = cursorIndex_;
            rebuildLineStarts();
            highlightDirty_ = true;
        }
        cursorBlinkTimer_ = 0.0f;
        ensureCursorVisibleV_ = true;
        ensureCursorVisibleH_ = true;
        updateAutoComplete();
    }

    void TextEditor::handleDeleteKey() {
        if (!ImGui::IsKeyPressed(ImGuiKey_Delete))
            return;

        pushUndoSnapshot();

        if (selectionActive_ && selectionStart() != selectionEnd()) {
            deleteSelection();
        } else if (cursorIndex_ < static_cast<int>(content_.size())) {
            // Find end of current character (UTF-8 aware)
            int nextPos = cursorIndex_ + 1;
            while (nextPos < static_cast<int>(content_.size()) &&
                   (static_cast<unsigned char>(content_[nextPos]) & 0xC0) == 0x80)
                ++nextPos;

            int deleteLen = nextPos - cursorIndex_;
            content_.erase(cursorIndex_, deleteLen);
            if (cursorIndex_ < static_cast<int>(colors_.size()))
                colors_.erase(colors_.begin() + cursorIndex_,
                              colors_.begin() +
                                  std::min(nextPos, static_cast<int>(colors_.size())));
            rebuildLineStarts();
            highlightDirty_ = true;
        }
        cursorBlinkTimer_ = 0.0f;
        updateAutoComplete();
    }

    void TextEditor::handleTabKey() {
        if (!ImGui::IsKeyPressed(ImGuiKey_Tab, false))
            return;
        if (autocompleteVisible_)
            return; // handled in autocomplete
        if (ImGui::GetIO().KeyCtrl)
            return;

        pushUndoSnapshot();

        if (selectionActive_ && selectionStart() != selectionEnd()) {
            // Indent/unindent selected lines
            int startLine = getLineFromPos(selectionStart());
            int endLine = getLineFromPos(selectionEnd());
            bool unindent = ImGui::GetIO().KeyShift;

            int offset = 0;
            for (int line = startLine; line <= endLine; ++line) {
                int linePos = lineStarts_[line] + offset;
                if (unindent) {
                    if (linePos < static_cast<int>(content_.size()) && content_[linePos] == '\t') {
                        content_.erase(linePos, 1);
                        if (linePos < static_cast<int>(colors_.size()))
                            colors_.erase(colors_.begin() + linePos);
                        --offset;
                    }
                } else {
                    content_.insert(linePos, "\t");
                    colors_.insert(colors_.begin() + linePos, palette_.text);
                    ++offset;
                }
            }
            cursorIndex_ += offset;
            selectionAnchor_ += (offset > 0) ? offset : 0;
        } else {
            if (ImGui::GetIO().KeyShift) {
                // Unindent current line
                int line = getLineFromPos(cursorIndex_);
                int linePos = lineStarts_[line];
                if (linePos < static_cast<int>(content_.size()) && content_[linePos] == '\t') {
                    content_.erase(linePos, 1);
                    if (linePos < static_cast<int>(colors_.size()))
                        colors_.erase(colors_.begin() + linePos);
                    if (cursorIndex_ > linePos)
                        --cursorIndex_;
                }
            } else {
                insertTextAtCursor("\t");
            }
        }
        rebuildLineStarts();
        highlightDirty_ = true;
        cursorBlinkTimer_ = 0.0f;
        ensureCursorVisibleH_ = true;
    }

    void TextEditor::handleArrowKeys() {
        bool shift = ImGui::GetIO().KeyShift;
        bool ctrl = ImGui::GetIO().KeyCtrl;

        auto startSelection = [&]() {
            if (!shift) {
                if (selectionActive_ && selectionStart() != selectionEnd()) {
                    // Collapse selection
                    clearSelection();
                }
            } else if (!selectionActive_) {
                selectionActive_ = true;
                selectionAnchor_ = cursorIndex_;
            }
        };

        auto finishMove = [&]() {
            if (!shift)
                selectionAnchor_ = cursorIndex_;
            cursorBlinkTimer_ = 0.0f;
        };

        // Left
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            startSelection();
            if (!shift && selectionActive_ && selectionStart() != selectionEnd()) {
                cursorIndex_ = selectionStart();
            } else if (ctrl) {
                // Word jump left
                if (cursorIndex_ > 0) {
                    --cursorIndex_;
                    while (cursorIndex_ > 0 && !isWordChar(content_[cursorIndex_]))
                        --cursorIndex_;
                    while (cursorIndex_ > 0 && isWordChar(content_[cursorIndex_ - 1]))
                        --cursorIndex_;
                }
            } else if (cursorIndex_ > 0) {
                --cursorIndex_;
                // Skip UTF-8 continuation bytes
                while (cursorIndex_ > 0 &&
                       (static_cast<unsigned char>(content_[cursorIndex_]) & 0xC0) == 0x80)
                    --cursorIndex_;
            }
            finishMove();
            preferredColumn_ = getColumnFromPos(cursorIndex_);
            ensureCursorVisibleH_ = true;
            ensureCursorVisibleV_ = true;
        }

        // Right
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            startSelection();
            if (!shift && selectionActive_ && selectionStart() != selectionEnd()) {
                cursorIndex_ = selectionEnd();
            } else if (ctrl) {
                // Word jump right
                int sz = static_cast<int>(content_.size());
                if (cursorIndex_ < sz) {
                    ++cursorIndex_;
                    while (cursorIndex_ < sz && !isWordChar(content_[cursorIndex_]))
                        ++cursorIndex_;
                    while (cursorIndex_ < sz && isWordChar(content_[cursorIndex_]))
                        ++cursorIndex_;
                }
            } else if (cursorIndex_ < static_cast<int>(content_.size())) {
                ++cursorIndex_;
                // Skip UTF-8 continuation bytes
                while (cursorIndex_ < static_cast<int>(content_.size()) &&
                       (static_cast<unsigned char>(content_[cursorIndex_]) & 0xC0) == 0x80)
                    ++cursorIndex_;
            }
            finishMove();
            preferredColumn_ = getColumnFromPos(cursorIndex_);
            ensureCursorVisibleH_ = true;
            ensureCursorVisibleV_ = true;
        }

        // Up
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (autocompleteVisible_)
                return;
            startSelection();
            int line = getLineFromPos(cursorIndex_);
            if (line > 0) {
                int targetLine = line - 1;
                cursorIndex_ = getPosFromLineColumn(targetLine, preferredColumn_);
            }
            finishMove();
            ensureCursorVisibleV_ = true;
        }

        // Down
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (autocompleteVisible_)
                return;
            startSelection();
            int line = getLineFromPos(cursorIndex_);
            if (line < static_cast<int>(lineStarts_.size()) - 1) {
                int targetLine = line + 1;
                cursorIndex_ = getPosFromLineColumn(targetLine, preferredColumn_);
            }
            finishMove();
            ensureCursorVisibleV_ = true;
        }
    }

    void TextEditor::handleHomeEnd() {
        bool shift = ImGui::GetIO().KeyShift;

        if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
            if (!shift)
                clearSelection();
            else if (!selectionActive_) {
                selectionActive_ = true;
                selectionAnchor_ = cursorIndex_;
            }
            int line = getLineFromPos(cursorIndex_);
            cursorIndex_ = lineStarts_[line];
            if (!shift)
                selectionAnchor_ = cursorIndex_;
            preferredColumn_ = 0;
            cursorBlinkTimer_ = 0.0f;
            ensureCursorVisibleH_ = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_End)) {
            if (!shift)
                clearSelection();
            else if (!selectionActive_) {
                selectionActive_ = true;
                selectionAnchor_ = cursorIndex_;
            }
            int line = getLineFromPos(cursorIndex_);
            cursorIndex_ = getLineEnd(line);
            if (!shift)
                selectionAnchor_ = cursorIndex_;
            preferredColumn_ = getColumnFromPos(cursorIndex_);
            cursorBlinkTimer_ = 0.0f;
            ensureCursorVisibleH_ = true;
        }
    }

    void TextEditor::handleSelectAll() {
        if (!ImGui::GetIO().KeyCtrl)
            return;
        if (!ImGui::IsKeyPressed(ImGuiKey_A))
            return;

        selectionActive_ = true;
        selectionAnchor_ = 0;
        cursorIndex_ = static_cast<int>(content_.size());
    }

    void TextEditor::handleClipboard() {
        if (!ImGui::GetIO().KeyCtrl)
            return;

        // Copy
        if (ImGui::IsKeyPressed(ImGuiKey_C)) {
            std::string sel = getSelectedText();
            if (!sel.empty())
                ImGui::SetClipboardText(sel.c_str());
        }

        // Cut
        if (ImGui::IsKeyPressed(ImGuiKey_X) && !readOnly_) {
            std::string sel = getSelectedText();
            if (!sel.empty()) {
                ImGui::SetClipboardText(sel.c_str());
                pushUndoSnapshot();
                deleteSelection();
                cursorBlinkTimer_ = 0.0f;
                ensureCursorVisibleV_ = true;
            }
        }

        // Paste
        if (ImGui::IsKeyPressed(ImGuiKey_V) && !readOnly_) {
            const char* clipboard = ImGui::GetClipboardText();
            if (clipboard && *clipboard) {
                pushUndoSnapshot();
                insertTextAtCursor(clipboard);
                cursorBlinkTimer_ = 0.0f;
                ensureCursorVisibleV_ = true;
                ensureCursorVisibleH_ = true;
                dismissAutoComplete();
            }
        }
    }

    void TextEditor::handleUndoRedo() {
        if (!ImGui::GetIO().KeyCtrl)
            return;
        if (readOnly_)
            return;

        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (ImGui::GetIO().KeyShift) {
                // Redo
                if (!redoStack_.empty()) {
                    undoStack_.push_back({content_, cursorIndex_});
                    auto& snap = redoStack_.back();
                    content_ = snap.content;
                    cursorIndex_ = std::min(snap.cursorIndex, static_cast<int>(content_.size()));
                    redoStack_.pop_back();
                    selectionAnchor_ = cursorIndex_;
                    clearSelection();
                    rebuildLineStarts();
                    highlightDirty_ = true;
                    lastSnapshotContent_ = content_;
                    ensureCursorVisibleV_ = true;
                }
            } else {
                // Undo
                // First save current state if it differs from last snapshot
                if (content_ != lastSnapshotContent_) {
                    undoStack_.push_back({lastSnapshotContent_, cursorIndex_});
                    lastSnapshotContent_ = content_;
                }
                if (!undoStack_.empty()) {
                    redoStack_.push_back({content_, cursorIndex_});
                    auto& snap = undoStack_.back();
                    content_ = snap.content;
                    cursorIndex_ = std::min(snap.cursorIndex, static_cast<int>(content_.size()));
                    undoStack_.pop_back();
                    selectionAnchor_ = cursorIndex_;
                    clearSelection();
                    rebuildLineStarts();
                    highlightDirty_ = true;
                    lastSnapshotContent_ = content_;
                    ensureCursorVisibleV_ = true;
                }
            }
        }
    }

    void TextEditor::toggleLineComment() {
        if (readOnly_)
            return;

        pushUndoSnapshot();

        // Determine affected line range
        int startLine, endLine;
        if (selectionActive_ && selectionStart() != selectionEnd()) {
            startLine = getLineFromPos(selectionStart());
            endLine = getLineFromPos(selectionEnd());
            // If selection ends at column 0, don't include that line
            if (endLine > startLine && selectionEnd() == lineStarts_[endLine])
                --endLine;
        } else {
            startLine = endLine = getLineFromPos(cursorIndex_);
        }

        // Check if all affected lines are already commented
        bool allCommented = true;
        for (int i = startLine; i <= endLine; ++i) {
            int ls = lineStarts_[i];
            int le = getLineEnd(i);
            // Skip whitespace
            int j = ls;
            while (j < le && (content_[j] == ' ' || content_[j] == '\t'))
                ++j;
            if (j >= le)
                continue; // blank line, skip
            if (j + 1 < le && content_[j] == '-' && content_[j + 1] == '-') {
                continue; // commented
            }
            allCommented = false;
            break;
        }

        // Apply or remove comments, tracking offset for cursor adjustment
        int cursorOffset = 0;
        for (int i = startLine; i <= endLine; ++i) {
            int ls = lineStarts_[i];
            int le = getLineEnd(i);
            int j = ls;
            while (j < le && (content_[j] == ' ' || content_[j] == '\t'))
                ++j;

            if (allCommented) {
                // Remove "-- " or "--"
                if (j + 1 < le && content_[j] == '-' && content_[j + 1] == '-') {
                    int removeLen = 2;
                    if (j + 2 < le && content_[j + 2] == ' ')
                        removeLen = 3;
                    content_.erase(j, removeLen);
                    if (cursorIndex_ > j)
                        cursorOffset -= removeLen;
                }
            } else {
                // Insert "-- "
                content_.insert(j, "-- ");
                if (cursorIndex_ >= j)
                    cursorOffset += 3;
            }

            // Rebuild after each line since offsets shift
            rebuildLineStarts();
        }

        cursorIndex_ =
            std::clamp(cursorIndex_ + cursorOffset, 0, static_cast<int>(content_.size()));
        selectionAnchor_ = cursorIndex_;
        selectionActive_ = false;
        highlightDirty_ = true;
    }

    // --- Mouse Input ---

    void TextEditor::handleMouseInput() {
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
            return;

        int charIndex = getCharIndexFromScreenPos(ImGui::GetMousePos());

        // Double click - word select
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            handleDoubleClick(charIndex);
            return;
        }

        // Single click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dismissAutoComplete();
            bool shift = ImGui::GetIO().KeyShift;
            if (shift) {
                if (!selectionActive_) {
                    selectionActive_ = true;
                    selectionAnchor_ = cursorIndex_;
                }
                cursorIndex_ = charIndex;
            } else {
                cursorIndex_ = charIndex;
                selectionAnchor_ = charIndex;
                selectionActive_ = false;
            }
            isDragging_ = true;
            preferredColumn_ = getColumnFromPos(cursorIndex_);
            cursorBlinkTimer_ = 0.0f;
            ensureCursorVisibleH_ = true;
        }

        // Drag
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && isDragging_) {
            selectionActive_ = true;
            cursorIndex_ = charIndex;
            preferredColumn_ = getColumnFromPos(cursorIndex_);
        }

        // Release
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            isDragging_ = false;
        }

        // Mouse wheel scroll
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            // Handled by ImGui's built-in scroll
        }
    }

    int TextEditor::getCharIndexFromScreenPos(ImVec2 screenPos) const {
        float spaceWidth = ImGui::CalcTextSize(" ").x;

        // Determine line from Y position
        int line = static_cast<int>((screenPos.y - textOrigin_.y + scrollY_) / lineHeight_);
        line = std::clamp(line, 0, static_cast<int>(lineStarts_.size()) - 1);

        int lineStart = lineStarts_[line];
        int lineEnd = getLineEnd(line);

        // Find character from X position
        float targetX = screenPos.x;
        float x = textOrigin_.x;

        for (int i = lineStart; i < lineEnd; ++i) {
            if (content_[i] == '\n')
                return i;

            float charW;
            if (content_[i] == '\t') {
                int col = static_cast<int>((x - textOrigin_.x) / spaceWidth);
                int nextTab = ((col / tabSize_) + 1) * tabSize_;
                charW = (nextTab * spaceWidth) - (x - textOrigin_.x);
            } else {
                const char* cs = &content_[i];
                const char* ce = cs + 1;
                if (static_cast<unsigned char>(*cs) >= 0x80) {
                    while (ce < content_.data() + content_.size() &&
                           (static_cast<unsigned char>(*ce) & 0xC0) == 0x80)
                        ++ce;
                }
                charW = ImGui::CalcTextSize(cs, ce).x;
                int bytes = static_cast<int>(ce - cs);
                if (bytes > 1)
                    i += bytes - 1;
            }

            if (targetX < x + charW * 0.5f)
                return i;
            x += charW;
        }

        return lineEnd;
    }

    void TextEditor::handleDoubleClick(int charIndex) {
        if (charIndex < 0 || charIndex >= static_cast<int>(content_.size()))
            return;

        int start = charIndex;
        int end = charIndex;

        // Find word boundaries
        while (start > 0 && isWordChar(content_[start - 1]))
            --start;
        while (end < static_cast<int>(content_.size()) && isWordChar(content_[end]))
            ++end;

        selectionActive_ = true;
        selectionAnchor_ = start;
        cursorIndex_ = end;
        preferredColumn_ = getColumnFromPos(cursorIndex_);
        cursorBlinkTimer_ = 0.0f;
    }

    // --- Autocomplete ---

    void TextEditor::updateAutoComplete() {
        // Build source items: use set items or fall back to default keywords
        const std::vector<CompletionItem>* items = &completionItems_;
        std::vector<CompletionItem> defaultItems;
        if (completionItems_.empty()) {
            const auto& kws = GetDefaultCompletionKeywords();
            defaultItems.reserve(kws.size());
            for (const auto& kw : kws)
                defaultItems.push_back({kw, CompletionKind::Keyword});
            items = &defaultItems;
        }

        std::string word = getCurrentWord();
        if (word.empty()) {
            dismissAutoComplete();
            return;
        }

        // Case-insensitive prefix match
        std::string lowerWord = word;
        std::transform(lowerWord.begin(), lowerWord.end(), lowerWord.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        filteredCompletions_.clear();
        for (const auto& item : *items) {
            std::string lowerText = item.text;
            std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lowerText.find(lowerWord) == 0 && lowerText != lowerWord)
                filteredCompletions_.push_back(item);
        }

        // Sort
        std::sort(filteredCompletions_.begin(), filteredCompletions_.end(),
                  [](const CompletionItem& a, const CompletionItem& b) {
                      return std::lexicographical_compare(
                          a.text.begin(), a.text.end(), b.text.begin(), b.text.end(),
                          [](char ca, char cb) { return std::tolower(ca) < std::tolower(cb); });
                  });

        if (filteredCompletions_.empty()) {
            dismissAutoComplete();
            return;
        }

        autocompleteVisible_ = true;
        autocompleteIndex_ = 0;
        autocompleteScrollOffset_ = 0;
        autocompleteWordStart_ = cursorIndex_ - static_cast<int>(word.size());
    }

    void TextEditor::renderAutoComplete() {
        // Re-evaluate on render so visibility doesn't depend solely on input events.
        if (!autocompleteVisible_ || filteredCompletions_.empty()) {
            updateAutoComplete();
        }
        if (!autocompleteVisible_ || filteredCompletions_.empty())
            return;

        float cursorX = getCursorScreenX();
        int line = getLineFromPos(cursorIndex_);
        float cursorY = textOrigin_.y + (line + 1) * lineHeight_;

        constexpr float popupWidth = 240.0f;
        constexpr float popupPadding = 6.0f;
        const int maxShow =
            std::min(static_cast<int>(filteredCompletions_.size()), kAutocompleteMaxVisible);
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float popupHeight = popupPadding * 2.0f + rowHeight * maxShow;

        ImGuiViewport* viewport = ImGui::GetWindowViewport();
        ImVec2 pos(cursorX, cursorY);
        if (viewport) {
            const ImVec2 vpMin = viewport->Pos;
            const ImVec2 vpMax(vpMin.x + viewport->Size.x, vpMin.y + viewport->Size.y);
            pos.x = std::clamp(pos.x, vpMin.x, vpMax.x - popupWidth);
            pos.y = std::clamp(pos.y, vpMin.y, vpMax.y - popupHeight);
        }

        ImDrawList* drawList =
            viewport ? ImGui::GetForegroundDrawList(viewport) : ImGui::GetForegroundDrawList();
        const ImVec2 rectMin = pos;
        const ImVec2 rectMax(pos.x + popupWidth, pos.y + popupHeight);
        drawList->AddRectFilled(rectMin, rectMax, palette_.background);
        drawList->AddRect(rectMin, rectMax, palette_.lineNumber);

        const ImU32 textColor = palette_.text;
        constexpr float iconWidth = 20.0f;
        for (int i = 0; i < maxShow; ++i) {
            const int itemIdx = i + autocompleteScrollOffset_;
            if (itemIdx >= static_cast<int>(filteredCompletions_.size()))
                break;
            const float rowY = pos.y + popupPadding + rowHeight * i;
            if (itemIdx == autocompleteIndex_) {
                drawList->AddRectFilled(ImVec2(pos.x + 2.0f, rowY),
                                        ImVec2(pos.x + popupWidth - 2.0f, rowY + rowHeight),
                                        palette_.selection);
            }

            // Icon label and color based on completion kind
            const char* icon = "K";
            ImU32 iconColor = palette_.keyword;
            switch (filteredCompletions_[itemIdx].kind) {
            case CompletionKind::Keyword:
                break;
            case CompletionKind::Table:
                icon = "T";
                iconColor = palette_.function;
                break;
            case CompletionKind::Column:
                icon = "C";
                iconColor = palette_.text;
                break;
            case CompletionKind::View:
                icon = "V";
                iconColor = palette_.string;
                break;
            case CompletionKind::Sequence:
                icon = "S";
                iconColor = palette_.number;
                break;
            case CompletionKind::Function:
                icon = "F";
                iconColor = palette_.type;
                break;
            }

            float iconX = pos.x + popupPadding;
            drawList->AddText(ImVec2(iconX, rowY), iconColor, icon);
            drawList->AddText(ImVec2(pos.x + popupPadding + iconWidth, rowY), textColor,
                              filteredCompletions_[itemIdx].text.c_str());
        }
    }

    void TextEditor::applyAutoComplete() {
        if (!autocompleteVisible_ || autocompleteIndex_ < 0 ||
            autocompleteIndex_ >= static_cast<int>(filteredCompletions_.size()))
            return;

        pushUndoSnapshot();

        const std::string& completion = filteredCompletions_[autocompleteIndex_].text;
        std::string word = getCurrentWord();

        // Replace the current word with the completion
        int wordStart = cursorIndex_ - static_cast<int>(word.size());
        content_.erase(wordStart, word.size());
        if (wordStart < static_cast<int>(colors_.size()))
            colors_.erase(colors_.begin() + wordStart,
                          colors_.begin() + std::min(wordStart + static_cast<int>(word.size()),
                                                     static_cast<int>(colors_.size())));

        content_.insert(wordStart, completion);
        colors_.insert(colors_.begin() + wordStart, completion.size(), palette_.text);

        cursorIndex_ = wordStart + static_cast<int>(completion.size());
        selectionAnchor_ = cursorIndex_;
        rebuildLineStarts();
        highlightDirty_ = true;
        dismissAutoComplete();
    }

    void TextEditor::dismissAutoComplete() {
        autocompleteVisible_ = false;
        filteredCompletions_.clear();
        autocompleteIndex_ = 0;
        autocompleteScrollOffset_ = 0;
    }

    // --- Find/Replace ---

    void TextEditor::openFind() {
        findVisible_ = true;
        findFocusRequested_ = true;
        // Pre-fill with selected text if any
        std::string sel = getSelectedText();
        if (!sel.empty() && sel.size() < sizeof(findBuf_) - 1) {
            strncpy(findBuf_, sel.c_str(), sizeof(findBuf_) - 1);
            findBuf_[sizeof(findBuf_) - 1] = '\0';
            performFind();
        }
    }

    void TextEditor::closeFind() {
        findVisible_ = false;
        findMatches_.clear();
        findCurrentMatch_ = -1;
    }

    void TextEditor::performFind() {
        findMatches_.clear();
        findCurrentMatch_ = -1;

        std::string needle(findBuf_);
        if (needle.empty())
            return;

        std::string haystack = content_;
        std::string needleLower = needle;
        if (!findCaseSensitive_) {
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::transform(needleLower.begin(), needleLower.end(), needleLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }

        int needleLen = static_cast<int>(needleLower.size());
        size_t pos = 0;
        while ((pos = haystack.find(needleLower, pos)) != std::string::npos) {
            if (findWholeWord_) {
                int ipos = static_cast<int>(pos);
                bool boundaryBefore = (ipos == 0) || !isWordChar(content_[ipos - 1]);
                bool boundaryAfter = (ipos + needleLen >= static_cast<int>(content_.size())) ||
                                     !isWordChar(content_[ipos + needleLen]);
                if (boundaryBefore && boundaryAfter)
                    findMatches_.push_back(ipos);
            } else {
                findMatches_.push_back(static_cast<int>(pos));
            }
            pos += needleLower.size();
        }

        // Select nearest match to cursor
        if (!findMatches_.empty()) {
            findCurrentMatch_ = 0;
            for (size_t i = 0; i < findMatches_.size(); ++i) {
                if (findMatches_[i] >= cursorIndex_) {
                    findCurrentMatch_ = static_cast<int>(i);
                    break;
                }
            }
            int matchPos = findMatches_[findCurrentMatch_];
            cursorIndex_ = matchPos + needleLen;
            selectionAnchor_ = matchPos;
            selectionActive_ = true;
            ensureCursorVisibleV_ = true;
            ensureCursorVisibleH_ = true;
        }
    }

    void TextEditor::findNext() {
        if (findMatches_.empty())
            return;
        findCurrentMatch_ = (findCurrentMatch_ + 1) % static_cast<int>(findMatches_.size());
        int matchPos = findMatches_[findCurrentMatch_];
        int len = static_cast<int>(strlen(findBuf_));
        cursorIndex_ = matchPos + len;
        selectionAnchor_ = matchPos;
        selectionActive_ = true;
        ensureCursorVisibleV_ = true;
        ensureCursorVisibleH_ = true;
        cursorBlinkTimer_ = 0.0f;
    }

    void TextEditor::findPrev() {
        if (findMatches_.empty())
            return;
        findCurrentMatch_ = (findCurrentMatch_ - 1 + static_cast<int>(findMatches_.size())) %
                            static_cast<int>(findMatches_.size());
        int matchPos = findMatches_[findCurrentMatch_];
        int len = static_cast<int>(strlen(findBuf_));
        cursorIndex_ = matchPos + len;
        selectionAnchor_ = matchPos;
        selectionActive_ = true;
        ensureCursorVisibleV_ = true;
        ensureCursorVisibleH_ = true;
        cursorBlinkTimer_ = 0.0f;
    }

    void TextEditor::replaceOne() {
        if (findMatches_.empty() || findCurrentMatch_ < 0)
            return;
        std::string needle(findBuf_);
        std::string replacement(replaceBuf_);
        if (needle.empty())
            return;

        pushUndoSnapshot();

        int matchPos = findMatches_[findCurrentMatch_];
        int needleLen = static_cast<int>(needle.size());

        content_.erase(matchPos, needleLen);
        content_.insert(matchPos, replacement);

        int diff = static_cast<int>(replacement.size()) - needleLen;
        cursorIndex_ = matchPos + static_cast<int>(replacement.size());
        selectionAnchor_ = cursorIndex_;
        selectionActive_ = false;

        rebuildLineStarts();
        highlightDirty_ = true;
        colors_.resize(content_.size(), palette_.text);

        // Re-run find to update matches
        performFind();
    }

    void TextEditor::replaceAll() {
        std::string needle(findBuf_);
        std::string replacement(replaceBuf_);
        if (needle.empty() || findMatches_.empty())
            return;

        pushUndoSnapshot();

        // Replace from end to start to preserve positions
        for (int i = static_cast<int>(findMatches_.size()) - 1; i >= 0; --i) {
            int pos = findMatches_[i];
            content_.erase(pos, needle.size());
            content_.insert(pos, replacement);
        }

        rebuildLineStarts();
        highlightDirty_ = true;
        colors_.resize(content_.size(), palette_.text);
        performFind();
    }

    void TextEditor::renderFindReplace() {
        if (!findVisible_)
            return;

        ImVec2 winPos = ImGui::GetWindowPos();
        float winWidth = ImGui::GetWindowWidth();

        float barWidth = std::min(460.0f, winWidth - 20.0f);
        float barX = winPos.x + winWidth - barWidth - 10.0f;
        float barY = winPos.y + 4.0f;

        ImGui::SetNextWindowPos(ImVec2(barX, barY));
        ImGui::SetNextWindowSize(ImVec2(barWidth, 0));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        // Derive panel colors from the editor palette
        ImU32 bgCol = palette_.currentLineBackground;
        int bgR = (bgCol >> 0) & 0xFF, bgG = (bgCol >> 8) & 0xFF, bgB = (bgCol >> 16) & 0xFF;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(bgR, bgG, bgB, 240));
        ImU32 lnCol = palette_.lineNumber;
        int lnR = (lnCol >> 0) & 0xFF, lnG = (lnCol >> 8) & 0xFF, lnB = (lnCol >> 16) & 0xFF;
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(lnR, lnG, lnB, 200));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("##find_replace", nullptr, flags)) {
            float rightControlsWidth = 140.0f;
            float inputWidth = barWidth - rightControlsWidth - 16.0f;

            // --- Find row ---
            ImGui::SetNextItemWidth(inputWidth);
            bool findEnter = ImGui::InputText("##find_input", findBuf_, sizeof(findBuf_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            if (findEnter) {
                performFind();
                findNext();
            }
            if (ImGui::IsItemEdited()) {
                performFind();
            }
            if (findFocusRequested_) {
                ImGui::SetKeyboardFocusHere(-1);
                findFocusRequested_ = false;
            }

            // Right side: match count, options, nav
            ImGui::SameLine();

            // Match count badge
            char countBuf[32];
            if (findBuf_[0] == '\0')
                countBuf[0] = '\0';
            else if (findMatches_.empty())
                snprintf(countBuf, sizeof(countBuf), "0");
            else
                snprintf(countBuf, sizeof(countBuf), "%d/%d", findCurrentMatch_ + 1,
                         static_cast<int>(findMatches_.size()));

            if (countBuf[0]) {
                bool noMatch = findMatches_.empty() && findBuf_[0] != '\0';
                if (noMatch)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(countBuf);
                if (noMatch)
                    ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            // Case sensitivity toggle
            bool csChanged = false;
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  findCaseSensitive_ ? palette_.selection : palette_.background);
            if (ImGui::SmallButton("Aa")) {
                findCaseSensitive_ = !findCaseSensitive_;
                csChanged = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Match Case (%s)", findCaseSensitive_ ? "on" : "off");
            ImGui::PopStyleColor();

            ImGui::SameLine();

            // Whole word toggle
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  findWholeWord_ ? palette_.selection : palette_.background);
            if (ImGui::SmallButton("W")) {
                findWholeWord_ = !findWholeWord_;
                csChanged = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Match Whole Word (%s)", findWholeWord_ ? "on" : "off");
            ImGui::PopStyleColor();

            if (csChanged)
                performFind();

            ImGui::SameLine();

            // Prev / Next
            if (ImGui::SmallButton("<"))
                findPrev();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Previous Match (Shift+F3)");
            ImGui::SameLine();
            if (ImGui::SmallButton(">"))
                findNext();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Next Match (F3)");

            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                closeFind();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Close (Escape)");

            // --- Replace row ---
            ImGui::SetNextItemWidth(inputWidth);
            if (ImGui::InputText("##replace_input", replaceBuf_, sizeof(replaceBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                replaceOne();
            }

            ImGui::SameLine();

            // Replace next
            bool noMatches = findMatches_.empty();
            if (noMatches)
                ImGui::BeginDisabled();
            if (ImGui::SmallButton("Replace"))
                replaceOne();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Replace Next Match");
            ImGui::SameLine();
            if (ImGui::SmallButton("All"))
                replaceAll();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Replace All Matches");
            if (noMatches)
                ImGui::EndDisabled();

            // Keyboard shortcuts
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                closeFind();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F3)) {
                if (ImGui::GetIO().KeyShift)
                    findPrev();
                else
                    findNext();
            }
        }
        ImGui::End();

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(5);
    }

    void TextEditor::renderFindHighlights(ImDrawList* drawList, int startLine, int endLine) {
        if (!findVisible_ || findMatches_.empty())
            return;

        int needleLen = static_cast<int>(strlen(findBuf_));
        if (needleLen == 0)
            return;

        float spaceWidth = ImGui::CalcTextSize(" ").x;
        // Derive highlight colors from palette (yellow-tinted from type color)
        int hR = (palette_.type >> 0) & 0xFF, hG = (palette_.type >> 8) & 0xFF,
            hB = (palette_.type >> 16) & 0xFF;
        ImU32 highlightColor = IM_COL32(hR, hG, hB, 60);
        ImU32 currentColor = IM_COL32(hR, hG, hB, 120);

        int startPos = (startLine >= 0 && startLine < static_cast<int>(lineStarts_.size()))
                           ? lineStarts_[startLine]
                           : 0;
        int endPos = (endLine >= 0 && endLine < static_cast<int>(lineStarts_.size()))
                         ? getLineEnd(endLine)
                         : static_cast<int>(content_.size());

        for (size_t mi = 0; mi < findMatches_.size(); ++mi) {
            int matchStart = findMatches_[mi];
            int matchEnd = matchStart + needleLen;
            if (matchEnd < startPos || matchStart > endPos)
                continue;

            bool isCurrent = (static_cast<int>(mi) == findCurrentMatch_);

            // Calculate screen positions for the match
            int mLine = getLineFromPos(matchStart);
            int lineStart = lineStarts_[mLine];
            float y = textOrigin_.y + mLine * lineHeight_;

            // Calculate x for match start
            auto calcX = [&](int pos) -> float {
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
            };

            float x1 = calcX(matchStart);
            float x2 = calcX(matchEnd);

            drawList->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + lineHeight_),
                                    isCurrent ? currentColor : highlightColor);
        }
    }

} // namespace dearsql
