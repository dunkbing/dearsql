#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include <string>
#include <vector>

class RedisDatabase;

class RedisKeyViewerTab final : public Tab {
public:
    RedisKeyViewerTab(const std::string& name, RedisDatabase* db, const std::string& pattern);
    ~RedisKeyViewerTab() override;

    void render() override;

    [[nodiscard]] const std::string& getPattern() const {
        return pattern_;
    }

private:
    RedisDatabase* db_;
    std::string pattern_;

    std::vector<std::string> columnNames_;
    std::vector<std::vector<std::string>> tableData_;

    int currentPage_ = 0;
    int rowsPerPage_ = 200;
    int totalRows_ = 0;

    bool isLoading_ = false;
    bool hasError_ = false;
    std::string loadingError_;

    AsyncOperation<std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>>
        loadOp_;

    void loadDataAsync();
    void checkLoadStatus();
    void renderToolbar();
};
