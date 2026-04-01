#pragma once

#include "database/table_data_provider.hpp"
#include "imgui.h"
#include <string>

enum class ExportFormat { CSV, JSON, SQL };

namespace TableExporter {
    bool exportTable(ITableDataProvider* provider, const Table& table, ExportFormat format);

    inline void renderExportMenu(ITableDataProvider* provider, const Table& table) {
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("CSV")) {
                exportTable(provider, table, ExportFormat::CSV);
            }
            if (ImGui::MenuItem("JSON")) {
                exportTable(provider, table, ExportFormat::JSON);
            }
            if (ImGui::MenuItem("SQL")) {
                exportTable(provider, table, ExportFormat::SQL);
            }
            ImGui::EndMenu();
        }
    }
} // namespace TableExporter
