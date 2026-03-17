#pragma once

#include <cstdint>
#include <string>

enum class TabType {
    SQL_EDITOR,
    TABLE_VIEWER,
    DIAGRAM,
    REDIS_EDITOR,
    REDIS_KEY_VIEWER,
    REDIS_PUBSUB,
    MONGO_EDITOR,
    CSV_EDITOR
};

class Tab {
public:
    Tab(std::string name, TabType type);
    virtual ~Tab() = default;

    // Common properties
    [[nodiscard]] std::uint64_t getId() const {
        return id_;
    }
    [[nodiscard]] const std::string& getName() const {
        return name;
    }
    void setName(const std::string& newName) {
        name = newName;
        refreshWindowName();
    }
    [[nodiscard]] const std::string& getWindowName() const {
        return windowName_;
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
    [[nodiscard]] virtual bool hasUnsavedChanges() const {
        return false;
    }

    // Virtual method for rendering tab content
    virtual void render() = 0;

protected:
    void refreshWindowName();

private:
    std::uint64_t id_ = 0;
    std::string name;
    std::string windowName_;
    TabType type;
    bool open = true;
};
