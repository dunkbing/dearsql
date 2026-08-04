#pragma once

#include "database/async_helper.hpp"
#include "database/db.hpp"
#include "ui/tab/tab.hpp"
#include "ui/text_editor.hpp"
#include <string>
#include <utility>

class IDatabaseNode;

class RoutineViewerTab final : public Tab {
public:
    RoutineViewerTab(IDatabaseNode* node, const Routine& routine);

    void render() override;

    [[nodiscard]] bool hasUnsavedChanges() const override {
        return contentModified_;
    }
    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
    }

    [[nodiscard]] IDatabaseNode* connectionNode() const override {
        return node_;
    }
    [[nodiscard]] const std::string& getRoutineName() const {
        return routine_.name;
    }
    [[nodiscard]] const std::string& getRoutineSignature() const {
        return routine_.signature;
    }
    [[nodiscard]] RoutineKind getRoutineKind() const {
        return routine_.kind;
    }

private:
    void fetchDefinitionAsync();
    void checkFetchStatus();
    void saveDefinitionAsync();
    void checkSaveStatus();
    void renderToolbar();
    [[nodiscard]] std::string buildDefinitionQuery() const;

    IDatabaseNode* node_ = nullptr;
    Routine routine_;
    dearsql::TextEditor editor_;

    // load state
    bool definitionLoaded_ = false;
    bool loadError_ = false;
    std::string loadErrorMessage_;
    AsyncOperation<std::string> fetchOp_;

    // edit/save state
    bool contentModified_ = false;
    AsyncOperation<std::pair<bool, std::string>> saveOp_;
    bool saveSuccess_ = false;
    std::string saveMessage_;
    float saveFeedbackTimer_ = 0.0f;
};
