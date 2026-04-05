#pragma once

#include "database/table_data_provider.hpp"
#include "imgui.h"

enum class ExportFormat { CSV, JSON, SQL };

namespace TableExporter {
    bool exportTables(ITableDataProvider* provider, const std::vector<const Table*>& tables,
                      ExportFormat format);

    inline void renderExportMenu(ITableDataProvider* provider, const Table& table) {
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("CSV")) {
                exportTables(provider, {&table}, ExportFormat::CSV);
            }
            if (ImGui::MenuItem("JSON")) {
                exportTables(provider, {&table}, ExportFormat::JSON);
            }
            if (ImGui::MenuItem("SQL")) {
                exportTables(provider, {&table}, ExportFormat::SQL);
            }
            ImGui::EndMenu();
        }
    }
} // namespace TableExporter
