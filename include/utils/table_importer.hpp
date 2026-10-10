#pragma once

#include "database/database_node.hpp"
#include "imgui.h"
#include <atomic>
#include <functional>
#include <optional>
#include <string>

// csv import into an existing table. the file is picked on the UI thread; the
// rows are inserted on a worker by importCsv
namespace TableImporter {
    struct Progress {
        std::atomic<long long> bytesRead{0};
        std::atomic<long long> totalBytes{0};
        std::atomic<long long> inserted{0};
        std::atomic<long long> failed{0};
        std::atomic<bool> cancelRequested{false};
    };

    struct Result {
        bool success = false; // every row landed
        bool cancelled = false;
        std::string error; // file-level failure, or the first row error
        std::string path;
        long long inserted = 0;
        long long failed = 0;
    };

    // UI thread: the file dialog; nullopt when the user backs out
    std::optional<std::string> chooseCsvFile();

    // any thread. rows go in multi-row INSERT batches where the dialect has them; a
    // batch that fails is retried row by row so one bad row only loses itself.
    // there is no wrapping transaction: rows already in stay in on cancel
    Result importCsv(IDatabaseNode* node, const std::string& tableName, const std::string& path,
                     Progress& progress);

    inline void renderImportMenu(const std::function<void()>& onCsv, bool enabled = true) {
        if (ImGui::BeginMenu("Import", enabled)) {
            if (ImGui::MenuItem("CSV"))
                onCsv();
            ImGui::EndMenu();
        }
    }
} // namespace TableImporter
