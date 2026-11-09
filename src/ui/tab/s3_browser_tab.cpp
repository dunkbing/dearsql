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

    // "Folder", or the extension in capitals ("PNG"), or "File" with none
    std::string fileType(bool folder, const std::string& key) {
        if (folder)
            return "Folder";
        const auto slash = key.rfind('/');
        const auto dot = key.rfind('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
            dot + 1 == key.size() || dot == (slash == std::string::npos ? 0 : slash + 1))
            return "File";
        std::string ext = key.substr(dot + 1);
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::toupper(c); });
        return ext;
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
    selection_.Clear();
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

void S3BrowserTab::upload(const std::string& path) {
    if (path.empty()) {
        FileDialog::openFile([this](const std::string& p) { upload(p); });
        return;
    }
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

std::optional<S3BrowserTab::Clip>& S3BrowserTab::clipboard() {
    static std::optional<Clip> clip; // ui thread only
    return clip;
}

std::vector<S3BrowserTab::Entry> S3BrowserTab::selectedEntries() {
    std::vector<Entry> out;
    void* it = nullptr;
    ImGuiID i = 0;
    while (selection_.GetNextSelectedItem(&it, &i)) {
        if (i < entries_.size())
            out.push_back(entries_[i]);
    }
    // listing order, not the order of clicks
    std::ranges::sort(out, [](const Entry& a, const Entry& b) { return a.key < b.key; });
    return out;
}

void S3BrowserTab::copySelection(bool cut) {
    auto entries = selectedEntries();
    if (entries.empty())
        return;
    status_ = std::format("{} {} item{}", cut ? "Cut" : "Copied", entries.size(),
                          entries.size() == 1 ? "" : "s");
    statusIsError_ = false;
    clipboard() = Clip{client_, bucket_, std::move(entries), cut};
}

void S3BrowserTab::paste() {
    auto& clip = clipboard();
    if (!clip || op_.isRunning())
        return;
    // CopyObject is server-side: both ends need the same endpoint and keys
    if (clip->client != client_) {
        status_ = "Paste works within one connection";
        statusIsError_ = true;
        return;
    }
    // plan the target keys here, where the folder's names are known
    std::set<std::string> taken;
    for (const auto& e : entries_)
        taken.insert(e.key);
    struct Step {
        std::string src, dst;
        bool folder;
    };
    std::vector<Step> steps;
    for (const auto& e : clip->entries) {
        std::string name = baseName(e.key);
        std::string dst = prefix_ + name + (e.folder ? "/" : "");
        const bool samePlace = clip->bucket == bucket_ && dst == e.key;
        if (samePlace && clip->cut)
            continue; // moving onto itself
        if (!samePlace && e.folder && clip->bucket == bucket_ && dst.starts_with(e.key)) {
            status_ = std::format("Can't paste '{}' into itself", name);
            statusIsError_ = true;
            return;
        }
        if (samePlace) {
            // a copy next to the original: "a copy.txt", "a copy 2.txt"
            const auto dot = e.folder ? std::string::npos : name.rfind('.');
            const std::string stem =
                dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
            const std::string ext = stem.size() == name.size() ? "" : name.substr(stem.size());
            for (int n = 1;; ++n) {
                dst = prefix_ + stem + (n == 1 ? " copy" : std::format(" copy {}", n)) + ext +
                      (e.folder ? "/" : "");
                if (!taken.contains(dst))
                    break;
            }
        }
        taken.insert(dst);
        steps.push_back({e.key, dst, e.folder});
    }
    const bool cut = clip->cut;
    const std::string srcBucket = clip->bucket;
    if (cut)
        clip.reset(); // a move happens once
    if (steps.empty())
        return;
    runOp(std::format("{} {} item{}", cut ? "Moving" : "Copying", steps.size(),
                      steps.size() == 1 ? "" : "s"),
          [client = client_, srcBucket, bucket = bucket_, steps, cut, cancel = cancel_] {
              OpResult r;
              int objects = 0;
              for (const auto& s : steps) {
                  if (cancel->load())
                      break;
                  if (s.folder) {
                      objects += client->copyPrefix(srcBucket, s.src, bucket, s.dst);
                      if (cut)
                          client->deletePrefix(srcBucket, s.src);
                  } else {
                      client->copyObject(srcBucket, s.src, bucket, s.dst);
                      ++objects;
                      if (cut)
                          client->deleteObject(srcBucket, s.src);
                  }
              }
              r.message = std::format("{} {} object{}", cut ? "Moved" : "Copied", objects,
                                      objects == 1 ? "" : "s");
              r.reload = true;
              return r;
          });
}

void S3BrowserTab::confirmDelete(std::vector<Entry> entries) {
    if (entries.empty())
        return;
    std::string what;
    if (entries.size() > 1)
        what = std::format("{} items (folders with everything in them)", entries.size());
    else if (entries[0].folder)
        what = std::format("the folder '{}' and everything in it", entries[0].key);
    else
        what = std::format("'{}'", entries[0].key);
    Alert::show("Delete", std::format("Delete {}? This cannot be undone.", what),
                {{"Cancel", [] {}, AlertButton::Style::Cancel},
                 {"Delete", [pending = pendingDelete_, entries] { *pending = entries; },
                  AlertButton::Style::Destructive}});
}

void S3BrowserTab::deleteEntries(std::vector<Entry> entries) {
    const std::string label = entries.size() == 1
                                  ? "Deleting " + baseName(entries[0].key)
                                  : std::format("Deleting {} items", entries.size());
    runOp(label, [client = client_, bucket = bucket_, entries = std::move(entries)] {
        OpResult r;
        int n = 0;
        for (const auto& e : entries) {
            if (e.folder) {
                n += client->deletePrefix(bucket, e.key);
            } else {
                client->deleteObject(bucket, e.key);
                ++n;
            }
        }
        r.message = std::format("Deleted {} object{}", n, n == 1 ? "" : "s");
        r.reload = true;
        return r;
    });
}

void S3BrowserTab::handleShortcuts() {
    const auto& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) ||
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    const bool mod = io.KeyCtrl || io.KeySuper;
    if (op_.isRunning())
        return;
    if (mod && ImGui::IsKeyPressed(ImGuiKey_C, false))
        copySelection(false);
    if (mod && ImGui::IsKeyPressed(ImGuiKey_X, false))
        copySelection(true);
    if (mod && ImGui::IsKeyPressed(ImGuiKey_V, false))
        paste();
    // delete, or cmd+backspace as in finder
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
        (mod && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        confirmDelete(selectedEntries());
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
            selection_.Clear();
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
        deleteEntries(std::move(**pendingDelete_));
        pendingDelete_->reset();
    }

    handleShortcuts();
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

    // copy, cut, paste, delete: on the selection, pasted into this folder
    const bool none = selection_.Size == 0;
    const bool busy = op_.isRunning();
    const auto& clip = clipboard();
#ifdef __APPLE__
    const char* mod = "Cmd";
#else
    const char* mod = "Ctrl";
#endif
    struct Action {
        const char* icon;
        bool enabled;
        std::string tip;
        std::function<void()> run;
    };
    const Action actions[] = {
        {ICON_FA_COPY, !none, std::format("Copy ({}+C)", mod), [this] { copySelection(false); }},
        {ICON_FA_SCISSORS, !none, std::format("Cut ({}+X)", mod), [this] { copySelection(true); }},
        {ICON_FA_PASTE, clip && !busy,
         clip ? std::format("Paste {} item{} here ({}+V)", clip->entries.size(),
                            clip->entries.size() == 1 ? "" : "s", mod)
              : std::string("Paste (copy or cut something first)"),
         [this] { paste(); }},
        {ICON_FA_TRASH_CAN, !none && !busy, "Delete", [this] { confirmDelete(selectedEntries()); }},
    };
    ImGui::SameLine(0, Theme::Spacing::L);
    for (size_t a = 0; a < std::size(actions); ++a) {
        if (a > 0)
            ImGui::SameLine(0, Theme::Spacing::XS);
        ImGui::PushID(static_cast<int>(a));
        ImGui::BeginDisabled(!actions[a].enabled);
        if (UIUtils::IconButton(actions[a].icon))
            actions[a].run();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", actions[a].tip.c_str());
        ImGui::PopID();
    }

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
                                      ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##s3_objects", 4, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Last modified", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableHeadersRow();

    // a selectable reaches half the item spacing past itself: 2x cell padding fills the row
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(Theme::Spacing::M, ImGui::GetStyle().CellPadding.y * 2));
    // drag across rows (or from the empty space below) to box-select
    auto* ms = ImGui::BeginMultiSelect(ImGuiMultiSelectFlags_BoxSelect1d |
                                           ImGuiMultiSelectFlags_ClearOnEscape |
                                           ImGuiMultiSelectFlags_ClearOnClickVoid,
                                       selection_.Size, static_cast<int>(entries_.size()));
    selection_.ApplyRequests(ms);
    std::optional<std::string> navigate;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entries_.size()));
    if (ms->RangeSrcItem != -1)
        clipper.IncludeItemByIndex(
            static_cast<int>(ms->RangeSrcItem)); // keep the shift anchor alive
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry& e = entries_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const std::string label =
                std::format("{} {}", e.folder ? ICON_FA_FOLDER : ICON_FA_FILE, baseName(e.key));
            ImGui::SetNextItemSelectionUserData(i);
            if (ImGui::Selectable(label.c_str(), selection_.Contains(static_cast<ImGuiID>(i)),
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (e.folder)
                        navigate = e.key;
                    else
                        download(e);
                }
            }
            if (UIUtils::BeginContextMenu("##s3_entry_menu")) {
                renderEntryMenu(e, navigate);
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(colors.subtext0, "%s", fileType(e.folder, e.key).c_str());
            ImGui::TableNextColumn();
            if (!e.folder)
                ImGui::TextUnformatted(humanSize(e.size).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(colors.subtext0, "%s", e.modified.c_str());
            ImGui::PopID();
        }
    }
    selection_.ApplyRequests(ImGui::EndMultiSelect());
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
    // these act on the whole selection
    const size_t n = static_cast<size_t>(selection_.Size);
    const std::string items = n > 1 ? std::format(" {} Items", n) : "";
    if (ImGui::MenuItem(("Copy" + items).c_str()))
        copySelection(false);
    if (ImGui::MenuItem(("Cut" + items).c_str()))
        copySelection(true);
    if (ImGui::MenuItem("Paste Here", nullptr, false, clipboard().has_value() && !busy))
        paste();
    ImGui::Separator();
    if (ImGui::MenuItem(n > 1 ? std::format("Delete {} Items...", n).c_str()
                              : (entry.folder ? "Delete Folder..." : "Delete..."),
                        nullptr, false, !busy))
        confirmDelete(selectedEntries());
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
    if (selection_.Size > 0)
        text += std::format(" · {} selected", selection_.Size);
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
