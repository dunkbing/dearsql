#include "ui/tab/tab.hpp"
#include "IconsFontAwesome6.h"
#include <atomic>
#include <format>
#include <utility>

namespace {
    std::atomic_uint64_t g_nextTabId{1};

    const char* tabIcon(TabType type) {
        switch (type) {
        case TabType::SQL_EDITOR:
        case TabType::REDIS_EDITOR:
        case TabType::MONGO_EDITOR:
            return ICON_FA_CODE;
        case TabType::TABLE_VIEWER:
            return ICON_FA_TABLE;
        case TabType::TABLE_EDITOR:
            return ICON_FA_PEN_TO_SQUARE;
        case TabType::DIAGRAM:
            return ICON_FA_DIAGRAM_PROJECT;
        case TabType::REDIS_KEY_VIEWER:
            return ICON_FA_KEY;
        case TabType::REDIS_PUBSUB:
            return ICON_FA_TOWER_BROADCAST;
        case TabType::CSV_EDITOR:
            return ICON_FA_FILE_CSV;
        case TabType::ROUTINE_VIEWER:
            return ICON_FA_GEAR;
        case TabType::SQLITE_SEQUENCE_VIEWER:
        case TabType::POSTGRES_SEQUENCE_VIEWER:
            return ICON_FA_ARROW_UP_1_9;
        }
        return "";
    }
} // namespace

Tab::Tab(std::string name, const TabType type)
    : id_(g_nextTabId.fetch_add(1, std::memory_order_relaxed)), name(std::move(name)), type(type) {
    refreshWindowName();
}

void Tab::refreshWindowName() {
    windowName_ = std::format("{} {}###tab_{}", tabIcon(type), name, id_);
}
