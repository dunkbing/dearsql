#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include <memory>
#include <string>
#include <vector>

class RedisDatabase;
class TableRenderer;

class RedisKeyViewerTab final : public Tab {
public:
    RedisKeyViewerTab(const std::string& name, RedisDatabase* db, const std::string& pattern);
    ~RedisKeyViewerTab() override;

    void render() override;

    [[nodiscard]] const std::string& getPattern() const {
        return pattern_;
    }
    [[nodiscard]] const RedisDatabase* getDatabase() const {
        return db_;
    }

private:
    RedisDatabase* db_;
    std::string pattern_;

    std::vector<std::string> columnNames_;
    std::vector<std::vector<std::string>> tableData_;
    std::vector<std::vector<std::string>> originalData_;
    std::vector<std::vector<bool>> editedCells_;
    std::vector<bool> isNewRow_;

    std::unique_ptr<TableRenderer> tableRenderer_;
    int selectedRow_ = -1;
    int selectedCol_ = -1;
    bool hasChanges_ = false;

    int currentPage_ = 0;
    int rowsPerPage_ = 200;
    int totalRows_ = 0;

    bool isLoading_ = false;
    bool hasError_ = false;
    std::string loadingError_;
    std::string saveError_;

    AsyncOperation<std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>>
        loadOp_;

    struct SaveResult {
        bool success = true;
        std::string errorMessage;
    };
    AsyncOperation<SaveResult> saveOp_;

    void initializeTableRenderer();
    void loadDataAsync();
    void checkLoadStatus();
    void saveChanges();
    void cancelChanges();
    void renderToolbar();
};
