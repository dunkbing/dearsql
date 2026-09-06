#pragma once

#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include "imgui.h"

enum class ExportFormat { CSV, JSON, SQL, MARKDOWN, HTML };

namespace TableExporter {
    bool exportTables(ITableDataProvider* provider, const std::vector<const Table*>& tables,
                      ExportFormat format, DatabaseType dbType = DatabaseType::SQLITE);

    inline void renderExportMenu(ITableDataProvider* provider, const Table& table,
                                 DatabaseType dbType = DatabaseType::SQLITE) {
        if (ImGui::BeginMenu("Export")) {
            if (ImGui::MenuItem("CSV")) {
                exportTables(provider, {&table}, ExportFormat::CSV, dbType);
            }
            if (ImGui::MenuItem("JSON")) {
                exportTables(provider, {&table}, ExportFormat::JSON, dbType);
            }
            if (ImGui::MenuItem("SQL")) {
                exportTables(provider, {&table}, ExportFormat::SQL, dbType);
            }
            if (ImGui::MenuItem("Markdown")) {
                exportTables(provider, {&table}, ExportFormat::MARKDOWN, dbType);
            }
            if (ImGui::MenuItem("HTML")) {
                exportTables(provider, {&table}, ExportFormat::HTML, dbType);
            }
            ImGui::EndMenu();
        }
    }
} // namespace TableExporter
