#include "ui/tab/redis_status_panel.hpp"
#include "application.hpp"
#include "database/redis.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <chrono>
#include <format>
#include <string_view>

namespace {
    constexpr auto kRefreshInterval = std::chrono::seconds(5);

    std::string formatCpu(const std::string& user, const std::string& sys) {
        try {
            const double userVal = std::stod(user);
            const double sysVal = std::stod(sys);
            return std::format("{:.2f}s user / {:.2f}s sys", userVal, sysVal);
        } catch (const std::exception&) {
            if (!user.empty() || !sys.empty()) {
                return std::format("{} / {}", user.empty() ? "-" : user, sys.empty() ? "-" : sys);
            }
            return "-";
        }
    }
} // namespace

RedisStatusPanel::RedisStatusPanel(RedisDatabase* db) : db_(db) {
    lastRefreshAt_ = std::chrono::steady_clock::time_point::min();
}

void RedisStatusPanel::setDatabase(RedisDatabase* db) {
    db_ = db;
}

void RedisStatusPanel::tick() {
    loadOp_.check([this](RedisServerStatusSnapshot result) {
        snapshot_ = std::move(result);
        loading_ = false;
        lastRefreshAt_ = std::chrono::steady_clock::now();
    });

    if (loading_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool stale = !snapshot_.ready || (now - lastRefreshAt_) >= kRefreshInterval;
    if (!stale) {
        return;
    }

    RedisDatabase* db = db_;
    if (!db) {
        snapshot_ = RedisServerStatusSnapshot{};
        snapshot_.error = "No Redis connection";
        snapshot_.ready = true;
        lastRefreshAt_ = now;
        return;
    }

    loading_ = loadOp_.start([db]() { return fetchStatus(db); });
}

std::string RedisStatusPanel::readFirstCell(const QueryResult& result) {
    if (result.empty()) {
        return "";
    }
    const auto& stmt = result[0];
    if (stmt.tableData.empty() || stmt.tableData[0].empty()) {
        return "";
    }
    return stmt.tableData[0][0];
}

std::string RedisStatusPanel::extractInfoValue(const std::string& info, const std::string& key) {
    const std::string prefix = key + ":";
    size_t start = 0;
    while (start < info.size()) {
        const size_t end = info.find('\n', start);
        const size_t len = (end == std::string::npos) ? (info.size() - start) : (end - start);
        std::string_view line(info.data() + start, len);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.starts_with(prefix)) {
            return std::string(line.substr(prefix.size()));
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return "";
}

std::string RedisStatusPanel::formatNow() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

RedisServerStatusSnapshot RedisStatusPanel::fetchStatus(RedisDatabase* db) {
    RedisServerStatusSnapshot snapshot;

    if (!db || !db->isConnected()) {
        snapshot.error = "Not connected to Redis";
        snapshot.ready = true;
        snapshot.lastUpdated = formatNow();
        return snapshot;
    }

    const auto infoRes = db->executeQuery("INFO");
    if (!infoRes.success()) {
        snapshot.error = infoRes.errorMessage().empty() ? "INFO failed" : infoRes.errorMessage();
        snapshot.ready = true;
        snapshot.lastUpdated = formatNow();
        return snapshot;
    }

    const auto info = readFirstCell(infoRes);
    const std::string cpuUser = extractInfoValue(info, "used_cpu_user");
    const std::string cpuSys = extractInfoValue(info, "used_cpu_sys");
    snapshot.cpu = formatCpu(cpuUser, cpuSys);

    const std::string usedMemoryHuman = extractInfoValue(info, "used_memory_human");
    const std::string usedMemory = extractInfoValue(info, "used_memory");
    snapshot.memory =
        !usedMemoryHuman.empty() ? usedMemoryHuman : (usedMemory.empty() ? "-" : usedMemory + " B");

    const std::string connectedClients = extractInfoValue(info, "connected_clients");
    snapshot.connectedClients = connectedClients.empty() ? "-" : connectedClients;

    const auto dbSizeRes = db->executeQuery("DBSIZE");
    if (dbSizeRes.success()) {
        const auto keyCount = readFirstCell(dbSizeRes);
        snapshot.totalKeys = keyCount.empty() ? "-" : keyCount;
    } else {
        snapshot.totalKeys = "-";
    }

    snapshot.error.clear();
    snapshot.ready = true;
    snapshot.lastUpdated = formatNow();
    return snapshot;
}

void RedisStatusPanel::renderPanel(float availableHeight, const char* panelId) {
    tick();

    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(Theme::Spacing::XL, Theme::Spacing::XL));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::L));
    if (ImGui::BeginChild(panelId, ImVec2(kFixedPanelWidth, availableHeight),
                          ImGuiChildFlags_Borders)) {
        constexpr float headerPadLeft = static_cast<float>(Theme::Spacing::L);
        constexpr float headerPadTop = static_cast<float>(Theme::Spacing::S);
        constexpr float leftIndent = static_cast<float>(Theme::Spacing::L);

        ImGui::Dummy(ImVec2(0.0f, headerPadTop));
        ImGui::Indent(headerPadLeft);
        ImGui::TextUnformatted("Server Status");
        ImGui::Unindent(headerPadLeft);
        ImGui::Separator();

        ImGui::Indent(leftIndent);

        if (!snapshot_.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
            ImGui::TextWrapped("%s", snapshot_.error.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Text("CPU: %s", snapshot_.cpu.c_str());
        ImGui::Text("Memory: %s", snapshot_.memory.c_str());
        ImGui::Text("Total Keys: %s", snapshot_.totalKeys.c_str());
        ImGui::Text("Connected Clients: %s", snapshot_.connectedClients.c_str());

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::Text("Last Updated: %s", snapshot_.lastUpdated.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent(leftIndent);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void RedisStatusPanel::renderToggleStrip(bool& panelOpen, float stripWidth, float availableHeight,
                                         const char* childId, const char* buttonId,
                                         const char* label) {
    const auto& colors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.surface0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild(childId, ImVec2(stripWidth, availableHeight), ImGuiChildFlags_None)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        drawList->AddLine(stripPos, ImVec2(stripPos.x, stripPos.y + availableHeight),
                          ImGui::GetColorU32(colors.overlay0), 1.0f);

        const ImVec2 textSize = ImGui::CalcTextSize(label);
        constexpr float padding = 6.0f;
        const float buttonW = stripWidth;
        const float buttonH = textSize.x + padding * 2.0f;

        ImGui::SetCursorScreenPos(ImVec2(stripPos.x, stripPos.y));
        ImGui::InvisibleButton(buttonId, ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            panelOpen = !panelOpen;
        }

        const ImVec2 btnMin = stripPos;
        const ImVec2 btnMax(stripPos.x + buttonW, stripPos.y + buttonH);
        if (panelOpen || hovered) {
            drawList->AddRectFilled(btnMin, btnMax, ImGui::GetColorU32(colors.surface1));
        }

        drawList->AddLine(ImVec2(btnMin.x, btnMax.y), btnMax, ImGui::GetColorU32(colors.overlay0),
                          1.0f);

        const float cx = stripPos.x + buttonW * 0.5f;
        const float cy = stripPos.y + buttonH * 0.5f;
        const float textX = cx - textSize.x * 0.5f;
        const float textY = cy - textSize.y * 0.5f;

        drawList->PushClipRectFullScreen();
        const int vtxBegin = drawList->VtxBuffer.Size;
        drawList->AddText(ImVec2(textX, textY),
                          ImGui::GetColorU32(hovered ? colors.text : colors.subtext0), label);
        const int vtxEnd = drawList->VtxBuffer.Size;
        for (int i = vtxBegin; i < vtxEnd; i++) {
            ImDrawVert& v = drawList->VtxBuffer[i];
            const float dx = v.pos.x - cx;
            const float dy = v.pos.y - cy;
            v.pos.x = cx - dy;
            v.pos.y = cy + dx;
        }
        drawList->PopClipRect();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
