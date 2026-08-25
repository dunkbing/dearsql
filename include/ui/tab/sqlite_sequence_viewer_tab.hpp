#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include <cstdint>
#include <string>

class FileDatabase;

class SQLiteSequenceViewerTab final : public Tab {
public:
    SQLiteSequenceViewerTab(FileDatabase* db, std::string sequenceName);

    void render() override;

    [[nodiscard]] FileDatabase* getDatabase() const {
        return db_;
    }
    [[nodiscard]] const std::string& getSequenceName() const {
        return sequenceName_;
    }

private:
    struct FetchResult {
        bool ok = false;
        std::string error;
        std::int64_t value = 0;
    };

    void fetchAsync();
    void checkFetchStatus();

    FileDatabase* db_ = nullptr;
    std::string sequenceName_;

    bool loaded_ = false;
    bool loadError_ = false;
    std::string loadErrorMessage_;

    // sqlite_sequence only stores name + seq. min/max/increment are fixed for AUTOINCREMENT.
    std::int64_t value_ = 0;
    static constexpr std::int64_t kMinValue = 0;
    static constexpr std::int64_t kMaxValue = 9223372036854775807LL;
    static constexpr std::int64_t kIncrement = 1;

    AsyncOperation<FetchResult> fetchOp_;
};
