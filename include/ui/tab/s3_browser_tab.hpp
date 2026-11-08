#pragma once

#include "database/async_helper.hpp"
#include "database/s3_client.hpp"
#include "imgui.h"
#include "ui/tab/tab.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// one bucket of an S3 connection: folders (key prefixes) and objects, paged, with
// upload, download, delete and share links. every request runs off the UI thread
class S3BrowserTab final : public Tab {
public:
    S3BrowserTab(const std::string& name, std::shared_ptr<const S3Client> client,
                 std::string connection, std::string bucket);
    ~S3BrowserTab() override;

    void render() override;

    [[nodiscard]] const std::string& connection() const {
        return connection_;
    }
    [[nodiscard]] const std::string& bucket() const {
        return bucket_;
    }
    // for tests
    [[nodiscard]] size_t selectedCount() const {
        return static_cast<size_t>(selection_.Size);
    }

private:
    struct Entry {
        std::string key; // full key; folders end in '/'
        bool folder = false;
        std::uint64_t size = 0;
        std::string modified;
    };
    struct ListResult {
        std::string prefix;
        bool append = false;
        S3Listing listing;
        std::string error;
    };
    // upload / download / delete / link / details; one at a time
    struct OpResult {
        std::string error;
        std::string message;   // shown in the status bar
        std::string clipboard; // copied on the UI thread
        std::vector<std::pair<std::string, std::string>> details;
        bool reload = false;
    };

    std::shared_ptr<const S3Client> client_;
    std::string connection_;
    std::string bucket_;
    std::string prefix_;

    AsyncOperation<ListResult> listOp_;
    std::vector<Entry> entries_;
    std::string nextToken_;
    std::string listError_;
    // indices into entries_; imgui's multi-select drives it (click, shift, cmd, drag, cmd-a)
    ImGuiSelectionBasicStorage selection_;

    AsyncOperation<OpResult> op_;
    std::string opLabel_;
    std::shared_ptr<std::atomic<double>> progress_ = std::make_shared<std::atomic<double>>(-1.0);
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::string status_;
    bool statusIsError_ = false;

    std::vector<std::pair<std::string, std::string>> details_;
    bool openDetails_ = false;
    // set by the confirm alert (which may outlive the tab on some platforms), run next frame
    std::shared_ptr<std::optional<std::vector<Entry>>> pendingDelete_ =
        std::make_shared<std::optional<std::vector<Entry>>>();

    // copy / cut: app-wide, so a paste can land in another tab of the same connection
    struct Clip {
        std::shared_ptr<const S3Client> client;
        std::string bucket;
        std::vector<Entry> entries;
        bool cut = false;
    };
    static std::optional<Clip>& clipboard();

    void list(bool append = false);
    void open(const std::string& prefix);
    void runOp(std::string label, std::function<OpResult()> work);

    void upload();
    void download(const Entry& entry);
    [[nodiscard]] std::vector<Entry> selectedEntries();
    void copySelection(bool cut);
    void paste();
    void confirmDelete(std::vector<Entry> entries);
    void deleteEntries(std::vector<Entry> entries);
    void handleShortcuts();
    void copyLink(const Entry& entry, unsigned seconds);
    void showDetails(const Entry& entry);

    void renderToolbar();
    void renderBreadcrumb();
    void renderTable();
    // a folder's Open sets navigate: opening clears the entries being drawn
    void renderEntryMenu(const Entry& entry, std::optional<std::string>& navigate);
    void renderStatusBar();
    void renderDetails();
};
