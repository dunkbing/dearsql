#pragma once

#include "database/async_helper.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class IDatabaseNode;
class PostgresSchemaNode;

class PostgresSequenceViewerTab final : public Tab {
public:
    PostgresSequenceViewerTab(PostgresSchemaNode* schema, std::string sequenceName);
    ~PostgresSequenceViewerTab() override;

    void render() override;
    [[nodiscard]] bool hasUnsavedChanges() const override;

    [[nodiscard]] IDatabaseNode* getDatabaseNode() const;
    [[nodiscard]] PostgresSchemaNode* getSchemaNode() const {
        return schema_;
    }
    [[nodiscard]] const std::string& getSequenceName() const {
        return sequenceName_;
    }

private:
    struct FetchResult {
        bool ok = false;
        std::string error;
        std::string dataType;
        std::string comment;
        std::int64_t lastValue = 0;
        std::int64_t startValue = 1;
        std::int64_t minValue = 1;
        std::int64_t maxValue = 9223372036854775807LL;
        std::int64_t incrementBy = 1;
        std::int64_t cacheSize = 1;
        bool cycled = false;
        std::int64_t objectId = 0;
        std::string owner;
    };

    void fetchAsync();
    void checkFetchStatus();
    void rebuildDdl();
    void renderToolbar();
    void saveChanges();
    void cancelChanges();
    void showSaveConfirmationDialog();
    void checkSQLExecutionStatus();
    [[nodiscard]] std::vector<std::string> generateUpdateSQL() const;
    [[nodiscard]] bool isDirty() const;

    PostgresSchemaNode* schema_ = nullptr;
    std::string sequenceName_;

    bool loaded_ = false;
    bool loadError_ = false;
    std::string loadErrorMessage_;
    std::string dataType_;

    char tableNameBuf_[256]{};
    char commentBuf_[1024]{};
    char lastValueBuf_[64]{};
    char startValueBuf_[64]{};
    char minValueBuf_[64]{};
    char maxValueBuf_[64]{};
    char incrementByBuf_[64]{};
    char cacheBuf_[64]{};
    bool cycled_ = false;
    char ownerBuf_[256]{};
    char objectIdBuf_[64]{};

    FetchResult original_;

    dearsql::TextEditor ddlEditor_;
    bool ddlEditorInitialized_ = false;

    // save flow
    std::vector<std::string> pendingUpdateSQL_;
    bool showSaveDialog_ = false;
    bool dialogOpened_ = false;
    dearsql::TextEditor saveDialogEditor_;
    AsyncOperation<std::pair<bool, std::string>> sqlExecutionOp_;

    AsyncOperation<FetchResult> fetchOp_;
};
