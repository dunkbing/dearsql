#include "ui/plan_view.hpp"

#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <algorithm>
#include <format>

namespace {
    std::string count(double v) {
        return v == static_cast<double>(static_cast<long long>(v))
                   ? std::format("{}", static_cast<long long>(v))
                   : std::format("{:.2f}", v);
    }

    // a planner off by 10x either way is worth a look: stale statistics, a bad join order
    bool misestimated(const dearsql::PlanNode& n) {
        if (!n.estimatedRows || !n.actualRows || n.loops.value_or(1.0) == 0.0) {
            return false;
        }
        const double est = std::max(*n.estimatedRows, 1.0);
        const double act = std::max(*n.actualRows, 1.0);
        return est / act >= 10.0 || act / est >= 10.0;
    }

    // sqlite reports only the tree: no estimates, costs or timings
    bool hasNumbers(const dearsql::PlanNode& n) {
        return n.estimatedRows || n.cost || n.totalTimeMs ||
               std::ranges::any_of(n.children, hasNumbers);
    }

    void row(const dearsql::PlanNode& n, const dearsql::Plan& plan, double totalMs, bool numbers,
             const std::vector<dearsql::PlanWarning>& warnings, int& id) {
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(id++);
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns;
        if (n.children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        const bool open = ImGui::TreeNodeEx("##plan_node", flags, "%s", n.operation.c_str());
        if (!n.properties.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& [key, value] : n.properties) {
                ImGui::TextColored(colors.mutedForeground, "%s:", key.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(value.c_str());
            }
            ImGui::EndTooltip();
        }
        if (!n.detail.empty()) {
            ImGui::SameLine(0, Theme::Spacing::M);
            ImGui::TextColored(colors.mutedForeground, "%s", n.detail.c_str());
        }
        for (const auto& w : warnings) {
            if (w.node != &n)
                continue;
            ImGui::SameLine(0, Theme::Spacing::M);
            ImGui::TextColored(colors.warning, ICON_FA_TRIANGLE_EXCLAMATION);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", w.message.c_str());
        }

        if (numbers) {
            // rows: estimated, or estimated → actual
            ImGui::TableNextColumn();
            std::string rows = n.estimatedRows ? count(*n.estimatedRows) : "";
            if (plan.analyzed && n.actualRows) {
                rows = std::format("{} \xE2\x86\x92 {}", rows.empty() ? "?" : rows,
                                   count(*n.actualRows));
            }
            if (misestimated(n)) {
                ImGui::TextColored(colors.warning, "%s", rows.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("The planner expected %s rows per loop and got %s",
                                      count(*n.estimatedRows).c_str(),
                                      count(*n.actualRows).c_str());
                }
            } else {
                ImGui::TextUnformatted(rows.c_str());
            }

            if (plan.analyzed) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n.loops ? count(*n.loops).c_str() : "");
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(n.cost ? std::format("{:.2f}", *n.cost).c_str() : "");

            // own time, with a bar for its share of the whole
            if (plan.analyzed) {
                ImGui::TableNextColumn();
                if (n.selfTimeMs) {
                    const float share =
                        totalMs > 0
                            ? static_cast<float>(std::clamp(*n.selfTimeMs / totalMs, 0.0, 1.0))
                            : 0.0f;
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float w = ImGui::GetContentRegionAvail().x;
                    ImVec4 bar = share >= 0.3f ? colors.destructive : colors.primary;
                    bar.w = 0.35f;
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        p, ImVec2(p.x + w * share, p.y + ImGui::GetTextLineHeight()),
                        ImGui::GetColorU32(bar), Theme::Radius::Sm);
                    ImGui::Text("%.3f ms  %.0f%%", *n.selfTimeMs, share * 100.0f);
                }
            }
        }
        if (open && !n.children.empty()) {
            for (const auto& child : n.children) {
                row(child, plan, totalMs, numbers, warnings, id);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
} // namespace

void PlanView::render(const dearsql::Plan& plan, bool& showRaw) {
    const auto& colors = Application::getInstance().getCurrentColors();
    if (plan.planningTimeMs || plan.executionTimeMs) {
        ImGui::TextColored(colors.mutedForeground, "Planning %.3f ms   Execution %.3f ms",
                           plan.planningTimeMs.value_or(0.0), plan.executionTimeMs.value_or(0.0));
        ImGui::SameLine(0, Theme::Spacing::L);
    }
    ImGui::Checkbox("Raw###plan_raw", &showRaw);
    ImGui::Spacing();

    if (showRaw) {
        // read-only: the buffer is never written
        ImGui::InputTextMultiline("##plan_raw_text", const_cast<char*>(plan.raw.c_str()),
                                  plan.raw.size() + 1, ImVec2(-1, -1),
                                  ImGuiInputTextFlags_ReadOnly);
        return;
    }

    // fixed rules over the tree: hints, so the user can judge
    const auto warnings = dearsql::reviewPlan(plan);
    for (const auto& w : warnings) {
        ImGui::TextColored(colors.warning, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine(0, Theme::Spacing::S);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(w.message.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!warnings.empty())
        ImGui::Spacing();

    const bool numbers = hasNumbers(plan.root);
    const int columns = !numbers ? 1 : plan.analyzed ? 5 : 3;
    if (!ImGui::BeginTable("##plan", columns,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_ScrollY,
                           ImVec2(-1, -1))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Operation", ImGuiTableColumnFlags_WidthStretch);
    if (numbers) {
        ImGui::TableSetupColumn(plan.analyzed ? "Rows (est \xE2\x86\x92 actual)" : "Rows (est)",
                                ImGuiTableColumnFlags_WidthFixed, 150.0f);
        if (plan.analyzed) {
            ImGui::TableSetupColumn("Loops", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        }
        ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        if (plan.analyzed) {
            ImGui::TableSetupColumn("Own time", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        }
    }
    ImGui::TableHeadersRow();
    const double totalMs = plan.executionTimeMs.value_or(plan.root.totalTimeMs.value_or(0.0));
    int id = 0;
    row(plan.root, plan, totalMs, numbers, warnings, id);
    ImGui::EndTable();
}
