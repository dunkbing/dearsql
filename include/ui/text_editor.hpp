#pragma once
#include "imgui.h"
#include "themes.hpp"
#include <functional>
#include <string>
#include <vector>

struct TSParser;
struct TSTree;
struct TSQuery;

namespace dearsql {

    class TextEditor {
    public:
        TextEditor();
        ~TextEditor();

        TextEditor(const TextEditor&) = delete;
        TextEditor& operator=(const TextEditor&) = delete;
        TextEditor(TextEditor&&) noexcept;
        TextEditor& operator=(TextEditor&&) noexcept;

        // --- Public API ---
        void SetText(const std::string& text);
        [[nodiscard]] std::string GetText() const;
        void Render(const char* label, ImVec2 size, bool border = false);
        void SetFocus();

        // Autocomplete
        void SetCompletionKeywords(const std::vector<std::string>& keywords);
        [[nodiscard]] static const std::vector<std::string>& GetDefaultCompletionKeywords();

        // Theme
        struct Palette {
            ImU32 background;
            ImU32 text;
            ImU32 keyword;
            ImU32 string;
            ImU32 number;
            ImU32 comment;
            ImU32 function;
            ImU32 type;
            ImU32 operator_;
            ImU32 cursor;
            ImU32 selection;
            ImU32 lineNumber;
            ImU32 currentLineNumber;
            ImU32 currentLineBackground;
            ImU32 lineNumberBackground;
        };

        void SetPalette(const Palette& palette);
        static Palette GetDarkPalette();
        static Palette GetLightPalette();
        static Palette FromTheme(const Theme::Colors& colors);

        // Options
        void SetShowLineNumbers(bool show);
        void SetTabSize(int size);
        void SetReadOnly(bool readOnly);

    private:
        // --- Content ---
        std::string content_;
        std::vector<ImU32> colors_;
        std::vector<int> lineStarts_;

        // --- Cursor ---
        int cursorIndex_ = 0;
        int preferredColumn_ = 0;
        float cursorBlinkTimer_ = 0.0f;

        // --- Selection ---
        int selectionAnchor_ = 0;
        bool selectionActive_ = false;
        bool isDragging_ = false;

        // --- Scroll ---
        float scrollX_ = 0.0f;
        float scrollY_ = 0.0f;
        bool ensureCursorVisibleV_ = false;
        bool ensureCursorVisibleH_ = false;

        // --- Layout cache ---
        float lineHeight_ = 0.0f;
        float lineNumberWidth_ = 0.0f;
        ImVec2 textOrigin_;

        // --- Options ---
        bool showLineNumbers_ = true;
        int tabSize_ = 4;
        bool readOnly_ = false;
        bool focusRequested_ = false;

        // --- Palette ---
        Palette palette_;

        // --- Undo/Redo ---
        struct Snapshot {
            std::string content;
            int cursorIndex;
        };
        std::vector<Snapshot> undoStack_;
        std::vector<Snapshot> redoStack_;
        std::string lastSnapshotContent_;

        // --- Autocomplete ---
        std::vector<std::string> completionKeywords_;
        std::vector<std::string> filteredCompletions_;
        bool autocompleteVisible_ = false;
        int autocompleteIndex_ = 0;
        int autocompleteWordStart_ = 0;

        // --- Tree-sitter ---
        TSParser* tsParser_ = nullptr;
        TSTree* tsTree_ = nullptr;
        TSQuery* tsQuery_ = nullptr;
        std::string tsPreviousContent_;
        bool highlightDirty_ = true;

        // --- Content manipulation ---
        void rebuildLineStarts();
        int getLineFromPos(int pos) const;
        int getColumnFromPos(int pos) const;
        int getPosFromLineColumn(int line, int column) const;
        int getLineEnd(int line) const;

        // --- Input handling (text_editor_input.cpp) ---
        void handleKeyboardInput();
        void handleCharacterInput();
        void handleEnterKey();
        void handleBackspaceKey();
        void handleDeleteKey();
        void handleTabKey();
        void handleArrowKeys();
        void handleHomeEnd();
        void handleSelectAll();
        void handleClipboard();
        void handleUndoRedo();
        void handleMouseInput();
        int getCharIndexFromScreenPos(ImVec2 screenPos) const;
        void handleDoubleClick(int charIndex);

        // --- Selection helpers ---
        int selectionStart() const;
        int selectionEnd() const;
        void deleteSelection();
        std::string getSelectedText() const;
        void clearSelection();

        // --- Cursor helpers ---
        void ensureCursorVisible();
        void insertTextAtCursor(const std::string& text);
        void pushUndoSnapshot();
        bool isWordChar(char c) const;
        int findWordStart(int pos) const;
        int findWordEnd(int pos) const;

        // --- Rendering (text_editor.cpp) ---
        void renderLineNumbers(ImDrawList* drawList, int startLine, int endLine);
        void renderText(ImDrawList* drawList, int startLine, int endLine);
        void renderCursor(ImDrawList* drawList);
        void renderSelection(ImDrawList* drawList, int startLine, int endLine);
        void renderCurrentLineHighlight(ImDrawList* drawList);

        // --- Autocomplete ---
        void updateAutoComplete();
        void renderAutoComplete();
        void applyAutoComplete();
        void dismissAutoComplete();
        std::string getCurrentWord() const;

        // --- Find/Replace ---
        bool findVisible_ = false;
        bool findFocusRequested_ = false;
        char findBuf_[256] = {};
        char replaceBuf_[256] = {};
        bool findCaseSensitive_ = false;
        bool findWholeWord_ = false;
        std::vector<int> findMatches_;
        int findCurrentMatch_ = -1;
        void openFind();
        void closeFind();
        void performFind();
        void findNext();
        void findPrev();
        void replaceOne();
        void replaceAll();
        void renderFindReplace();
        void renderFindHighlights(ImDrawList* drawList, int startLine, int endLine);

        // --- Tree-sitter (text_editor_highlight.cpp) ---
        void initTreeSitter();
        void cleanupTreeSitter();
        void rehighlight();

        // --- Helpers ---
        float getCharWidth(const char* start, const char* end) const;
        float getCursorScreenX() const;
    };

} // namespace dearsql
