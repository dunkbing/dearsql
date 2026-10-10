#pragma once

#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include "imgui.h"
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class ExportFormat { CSV, JSON, SQL, MARKDOWN, HTML };

// table export (csv, json, sql, markdown, html). the destination is picked on
// the UI thread; the rows are paged out on a worker by run()
namespace TableExporter {
    struct Request {
        ExportFormat format = ExportFormat::CSV;
        DatabaseType dbType = DatabaseType::SQLITE;
        std::vector<Table> tables; // copies: the node may reload its list meanwhile
        std::string path;          // a file, or a folder when several tables go one per file
    };

    struct Progress {
        std::atomic<int> tablesDone{0};
        std::atomic<int> tablesTotal{0};
        std::atomic<long long> rowsWritten{0};
        std::atomic<long long> rowsTotal{0};        // of the table being written
        std::atomic<long long> tableRowsWritten{0}; // of the table being written
        std::atomic<bool> cancelRequested{false};
    };

    struct Result {
        bool success = false;
        bool cancelled = false;
        std::string error;
        std::string path;
        int tables = 0;
        long long rows = 0;
    };

    // UI thread: asks where to write; nullopt when the user backs out
    std::optional<Request> chooseDestination(const std::vector<const Table*>& tables,
                                             ExportFormat format, DatabaseType dbType);

    // any thread: writes every table, polling progress.cancelRequested between pages
    Result run(ITableDataProvider* provider, const Request& request, Progress& progress);

    // the Export submenu; onPick runs with the chosen format
    inline void renderExportMenu(const std::function<void(ExportFormat)>& onPick,
                                 bool enabled = true) {
        if (ImGui::BeginMenu("Export", enabled)) {
            if (ImGui::MenuItem("CSV"))
                onPick(ExportFormat::CSV);
            if (ImGui::MenuItem("JSON"))
                onPick(ExportFormat::JSON);
            if (ImGui::MenuItem("SQL"))
                onPick(ExportFormat::SQL);
            if (ImGui::MenuItem("Markdown"))
                onPick(ExportFormat::MARKDOWN);
            if (ImGui::MenuItem("HTML"))
                onPick(ExportFormat::HTML);
            ImGui::EndMenu();
        }
    }
} // namespace TableExporter
