#pragma once

#include "database/table_data_provider.hpp"
#include "imgui.h"
#include <string>

enum class ExportFormat { CSV, JSON };

namespace TableExporter {
    bool exportTable(ITableDataProvider* provider, const std::string& tableName,
                     ExportFormat format);

    inline void renderExportMenu(ITableDataProvider* provider, const std::string& tableName) {
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("CSV")) {
                exportTable(provider, tableName, ExportFormat::CSV);
            }
            if (ImGui::MenuItem("JSON")) {
                exportTable(provider, tableName, ExportFormat::JSON);
            }
            ImGui::EndMenu();
        }
    }
} // namespace TableExporter
