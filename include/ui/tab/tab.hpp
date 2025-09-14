#pragma once

#include <string>

enum class TabType { SQL_EDITOR, TABLE_VIEWER, DIAGRAM };

class Tab {
public:
    Tab(std::string name, TabType type);
    virtual ~Tab() = default;

    // Common properties
    [[nodiscard]] const std::string& getName() const {
        return name;
    }
    void setName(const std::string& newName) {
        name = newName;
    }
    [[nodiscard]] TabType getType() const {
        return type;
    }
    [[nodiscard]] bool isOpen() const {
        return open;
    }
    void setOpen(const bool isOpen) {
        open = isOpen;
    }
    [[nodiscard]] bool shouldFocus() const {
        return needsFocus;
    }
    void setShouldFocus(const bool focus) {
        needsFocus = focus;
    }

    // Virtual method for rendering tab content
    virtual void render() = 0;

protected:
    std::string name;
    TabType type;
    bool open = true;
    bool needsFocus = false;
};
