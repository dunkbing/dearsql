#include "ui/tab/s3_browser_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "platform/alert.hpp"
#include "themes.hpp"
#include "utils/button.hpp"
#include "utils/file_dialog.hpp"
#include "utils/menu.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <spdlog/spdlog.h>

namespace {
    std::string humanSize(std::uint64_t bytes) {
        constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        double v = static_cast<double>(bytes);
        int u = 0;
        while (v >= 1024.0 && u < 4) {
            v /= 1024.0;
            ++u;
        }
        return u == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", v, units[u]);
    }

    // last path segment: "a/b/c.txt" -> "c.txt", "a/b/" -> "b"
    std::string baseName(std::string key) {
        if (key.ends_with('/'))
            key.pop_back();
        const auto slash = key.rfind('/');
        return slash == std::string::npos ? key : key.substr(slash + 1);
    }
} // namespace

S3BrowserTab::S3BrowserTab(const std::string& name, std::shared_ptr<const S3Client> client,
                           std::string connection, std::string bucket)
    : Tab(name, TabType::S3_BROWSER), client_(std::move(client)),
      connection_(std::move(connection)), bucket_(std::move(bucket)) {
    list();
}

S3BrowserTab::~S3BrowserTab() {
    cancel_->store(true); // a running transfer stops at its next progress callback
    // the rest run to completion off the ui thread: workers hold only copies, never the tab
    listOp_.detach();
    op_.detach();
}

void S3BrowserTab::list(bool append) {
    if (listOp_.isRunning())
        return;
    listError_.clear();
    listOp_.start([client = client_, bucket = bucket_, prefix = prefix_,
                   token = append ? nextToken_ : std::string{}, append] {
        ListResult r;
        r.prefix = prefix;
        r.append = append;
        try {
            r.listing = client->listObjects(bucket, prefix, token);
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        return r;
    });
}

void S3BrowserTab::open(const std::string& prefix) {
    if (listOp_.isRunning())
        return;
    prefix_ = prefix;
    entries_.clear();
    nextToken_.clear();
    selected_ = -1;
    list();
}

void S3BrowserTab::runOp(std::string label, std::function<OpResult()> work) {
    if (op_.isRunning())
        return;
    opLabel_ = std::move(label);
    progress_->store(-1.0);
    cancel_->store(false);
    status_.clear();
    op_.start([work = std::move(work)] {
        try {
            return work();
        } catch (const std::exception& e) {
            OpResult r;
            r.error = e.what();
            return r;
        }
    });
}

static S3Client::Progress makeProgress(std::shared_ptr<std::atomic<double>> progress,
                                       std::shared_ptr<std::atomic<bool>> cancel) {
    return [progress, cancel](double done, double total) {
        if (total > 0)
            progress->store(done / total);
        return !cancel->load();
    };
}

void S3BrowserTab::upload() {
    const std::string path = FileDialog::openFile();
    if (path.empty())
        return;
    const std::string key = prefix_ + std::filesystem::path(path).filename().string();
    runOp("Uploading " + baseName(key), [client = client_, bucket = bucket_, key, path,
                                         progress = makeProgress(progress_, cancel_)] {
        client->upload(bucket, key, path, progress);
        OpResult r;
        r.message = "Uploaded " + key;
        r.reload = true;
        return r;
    });
}

void S3BrowserTab::download(const Entry& entry) {
    if (entry.folder)
        return;
    const std::string path = FileDialog::saveFile(baseName(entry.key), nullptr, nullptr);
    if (path.empty())
        return;
    runOp("Downloading " + baseName(entry.key),
          [client = client_, bucket = bucket_, key = entry.key, path,
           progress = makeProgress(progress_, cancel_)] {
              client->download(bucket, key, path, progress);
              OpResult r;
              r.message = "Saved " + path;
              return r;
          });
}

void S3BrowserTab::confirmDelete(const Entry& entry) {
    const std::string what = entry.folder
                                 ? std::format("the folder '{}' and everything in it", entry.key)
                                 : std::format("'{}'", entry.key);
    Alert::show("Delete", std::format("Delete {}? This cannot be undone.", what),
                {{"Cancel", [] {}, AlertButton::Style::Cancel},
                 {"Delete", [pending = pendingDelete_, entry] { *pending = entry; },
                  AlertButton::Style::Destructive}});
}

void S3BrowserTab::deleteEntry(const Entry& entry) {
    runOp("Deleting " + baseName(entry.key), [client = client_, bucket = bucket_, entry] {
        OpResult r;
        if (entry.folder) {
            const int n = client->deletePrefix(bucket, entry.key);
            r.message = std::format("Deleted {} objects", n);
        } else {
            client->deleteObject(bucket, entry.key);
            r.message = "Deleted " + entry.key;
        }
        r.reload = true;
        return r;
    });
}

void S3BrowserTab::copyLink(const Entry& entry, unsigned seconds) {
    runOp("Signing link", [client = client_, bucket = bucket_, key = entry.key, seconds] {
        OpResult r;
        r.clipboard = client->presignGet(bucket, key, seconds);
        r.message = "Link copied";
        return r;
    });
}

void S3BrowserTab::showDetails(const Entry& entry) {
    runOp("Reading details", [client = client_, bucket = bucket_, key = entry.key] {
        OpResult r;
        r.details = client->stat(bucket, key);
        r.details.insert(r.details.begin(), {"Key", key});
        return r;
    });
}

// ---------------------------------------------------------------- render

void S3BrowserTab::render() {
    listOp_.check([this](ListResult r) {
        if (r.prefix != prefix_)
            return; // the user moved on
        if (!r.error.empty()) {
            listError_ = r.error;
            return;
        }
        if (!r.append) {
            entries_.clear();
            selected_ = -1;
        }
        for (auto& p : r.listing.prefixes)
            entries_.push_back({std::move(p), true, 0, {}});
        for (auto& o : r.listing.objects)
            entries_.push_back({std::move(o.key), false, o.size, std::move(o.lastModified)});
        // folders first, then by name; stable so appended pages keep their place
        std::ranges::stable_sort(entries_, [](const Entry& a, const Entry& b) {
            return a.folder != b.folder ? a.folder : a.key < b.key;
        });
        nextToken_ = std::move(r.listing.nextToken);
    });
    op_.check([this](OpResult r) {
        if (!r.error.empty()) {
            spdlog::error("S3 {} failed: {}", opLabel_, r.error);
            status_ = r.error;
            statusIsError_ = true;
            return;
        }
        status_ = r.message;
        statusIsError_ = false;
        if (!r.clipboard.empty())
            ImGui::SetClipboardText(r.clipboard.c_str());
        if (!r.details.empty()) {
            details_ = std::move(r.details);
            openDetails_ = true;
        }
        if (r.reload)
            open(prefix_);
    });

    if (pendingDelete_->has_value()) {
        deleteEntry(**pendingDelete_);
        pendingDelete_->reset();
    }

    renderToolbar();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));

    if (ImGui::BeginChild("##s3_body"))
        renderTable();
    ImGui::EndChild();
    renderDetails();
}

void S3BrowserTab::renderToolbar() {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::BeginDisabled(prefix_.empty() || listOp_.isRunning());
    if (UIUtils::IconButton(ICON_FA_ARROW_UP)) {
        std::string parent = prefix_.substr(0, prefix_.size() - 1);
        const auto slash = parent.rfind('/');
        open(slash == std::string::npos ? "" : parent.substr(0, slash + 1));
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Up one folder");

    ImGui::SameLine(0, Theme::Spacing::XS);
    ImGui::BeginDisabled(listOp_.isRunning());
    if (UIUtils::IconButton(ICON_FA_ARROWS_ROTATE))
        open(prefix_);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Refresh");

    ImGui::SameLine(0, Theme::Spacing::XS);
    ImGui::BeginDisabled(op_.isRunning());
    if (UIUtils::IconButton(ICON_FA_UPLOAD))
        upload();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Upload a file here");

    if (!prefix_.empty()) {
        ImGui::SameLine(0, Theme::Spacing::L);
        renderBreadcrumb();
    }
    ImGui::SameLine(0, Theme::Spacing::L);
    renderStatusBar();

    if (op_.isRunning()) {
        ImGui::SameLine(0, Theme::Spacing::L);
        UIUtils::Spinner("##s3_op", 6.0f, 2, ImGui::GetColorU32(colors.blue));
        ImGui::SameLine(0, Theme::Spacing::S);
        const double p = progress_->load();
        if (p >= 0)
            ImGui::TextColored(colors.subtext0, "%s  %d%%", opLabel_.c_str(),
                               static_cast<int>(p * 100));
        else
            ImGui::TextColored(colors.subtext0, "%s", opLabel_.c_str());
        if (p >= 0) {
            ImGui::SameLine(0, Theme::Spacing::S);
            if (UIUtils::IconButton(ICON_FA_XMARK))
                cancel_->store(true);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cancel");
        }
    }
}

void S3BrowserTab::renderBreadcrumb() {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::AlignTextToFramePadding();
    // a / b, each a link to that folder; the tab title names the bucket
    auto crumb = [&](const std::string& label, const std::string& target, bool last) {
        if (last) {
            ImGui::TextUnformatted(label.c_str());
            return;
        }
        ImGui::TextColored(colors.blue, "%s", label.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked())
            open(target);
    };
    std::size_t start = 0;
    while (start < prefix_.size()) {
        const auto end = prefix_.find('/', start);
        if (start > 0) {
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, Theme::Spacing::S);
        }
        crumb(prefix_.substr(start, end - start), prefix_.substr(0, end + 1),
              end + 1 >= prefix_.size());
        start = end + 1;
    }
}

void S3BrowserTab::renderTable() {
    const auto& colors = Application::getInstance().getCurrentColors();

    if (!listError_.empty()) {
        ImGui::TextColored(colors.red, "%s", listError_.c_str());
        if (UIUtils::Button(ICON_FA_ARROWS_ROTATE " Retry"))
            list();
        return;
    }
    if (entries_.empty()) {
        if (listOp_.isRunning()) {
            UIUtils::Spinner("##s3_loading", 8.0f, 2, ImGui::GetColorU32(colors.peach));
            ImGui::SameLine(0, Theme::Spacing::M);
            ImGui::TextColored(colors.subtext0, "Loading...");
        } else {
            ImGui::TextDisabled(prefix_.empty() ? "This bucket is empty."
                                                : "This folder is empty.");
        }
        return;
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##s3_objects", 3, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Last modified", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableHeadersRow();

    // a selectable reaches half the item spacing past itself: 2x cell padding fills the row
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(Theme::Spacing::M, ImGui::GetStyle().CellPadding.y * 2));
    std::optional<std::string> navigate;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entries_.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry& e = entries_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const std::string label =
                std::format("{} {}", e.folder ? ICON_FA_FOLDER : ICON_FA_FILE, baseName(e.key));
            if (ImGui::Selectable(label.c_str(), selected_ == i,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_ = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (e.folder)
                        navigate = e.key;
                    else
                        download(e);
                }
            }
            if (UIUtils::BeginContextMenu("##s3_entry_menu")) {
                selected_ = i;
                renderEntryMenu(e, navigate);
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (!e.folder)
                ImGui::TextUnformatted(humanSize(e.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(colors.subtext0, "%s", e.modified.c_str());
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();

    if (!nextToken_.empty()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(listOp_.isRunning());
        if (UIUtils::Button("Load more"))
            list(true);
        ImGui::EndDisabled();
    }
    ImGui::EndTable();

    if (navigate)
        open(*navigate);
}

void S3BrowserTab::renderEntryMenu(const Entry& entry, std::optional<std::string>& navigate) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    const bool busy = op_.isRunning();
    if (entry.folder) {
        if (ImGui::MenuItem("Open"))
            navigate = entry.key;
    } else {
        if (ImGui::MenuItem("Download...", nullptr, false, !busy))
            download(entry);
        if (ImGui::MenuItem("Details", nullptr, false, !busy))
            showDetails(entry);
        if (UIUtils::BeginSubMenu("Copy Share Link", !busy)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(Theme::Spacing::M, Theme::Spacing::M));
            if (ImGui::MenuItem("Valid for 1 hour"))
                copyLink(entry, 3600);
            if (ImGui::MenuItem("Valid for 1 day"))
                copyLink(entry, 86400);
            if (ImGui::MenuItem("Valid for 7 days"))
                copyLink(entry, 604800);
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
    }
    if (ImGui::MenuItem("Copy Key"))
        ImGui::SetClipboardText(entry.key.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(entry.folder ? "Delete Folder..." : "Delete...", nullptr, false, !busy))
        confirmDelete(entry);
    ImGui::PopStyleVar();
}

void S3BrowserTab::renderStatusBar() {
    const auto& colors = Application::getInstance().getCurrentColors();
    int folders = 0;
    for (const auto& e : entries_)
        folders += e.folder ? 1 : 0;
    const int files = static_cast<int>(entries_.size()) - folders;
    std::string text = std::format("{} folders · {} files{}", folders, files,
                                   nextToken_.empty() ? "" : " (more not loaded)");
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colors.subtext0, "%s", text.c_str());
    if (!status_.empty()) {
        ImGui::SameLine(0, Theme::Spacing::M);
        ImGui::TextColored(statusIsError_ ? colors.red : colors.green, "%s", status_.c_str());
    }
}

void S3BrowserTab::renderDetails() {
    const char* id = "Object Details##s3_details";
    if (openDetails_) {
        ImGui::OpenPopup(id);
        openDetails_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::L));
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginTable("##s3_details_table", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (const auto& [k, v] : details_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (UIUtils::Button("Close", UIUtils::ButtonVariant::Primary) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
