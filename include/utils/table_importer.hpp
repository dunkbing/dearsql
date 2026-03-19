#pragma once

#include "database/query_executor.hpp"
#include "imgui.h"
#include <string>

namespace TableImporter {
    bool importFromCSV(IQueryExecutor* executor, const std::string& tableName);

    inline void renderImportMenu(IQueryExecutor* executor, const std::string& tableName) {
        if (ImGui::BeginMenu("Import")) {
            if (ImGui::MenuItem("CSV")) {
                importFromCSV(executor, tableName);
            }
            ImGui::EndMenu();
        }
    }
} // namespace TableImporter
