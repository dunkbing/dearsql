#include "ui/settings_dialog.hpp"

#include "application.hpp"
#include "imgui.h"
#include "platform/updater.hpp"
#include "themes.hpp"
#include "utils/button.hpp"

#include <cfloat>

#if defined(__APPLE__)
#include "ui/custom_shader.hpp"
#include "ui/text_editor.hpp"
#include "utils/file_dialog.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace {
    constexpr const char* POPUP_ID = "Settings";
} // namespace

SettingsDialog::SettingsDialog() = default;
SettingsDialog::~SettingsDialog() = default;

SettingsDialog& SettingsDialog::instance() {
    static SettingsDialog dialog;
    return dialog;
}

void SettingsDialog::open() {
    open_ = true;
    pendingOpen_ = true;
#if defined(__APPLE__)
    loadShaderState();
#endif
}

#if defined(__APPLE__)
void SettingsDialog::loadShaderState() {
    if (!shaderEditor_) {
        shaderEditor_ = std::make_unique<dearsql::TextEditor>();
        shaderEditor_->SetLanguage(dearsql::TextEditor::Language::PlainText);
    }

    auto* appState = Application::getInstance().getAppState();
    std::string defaultPath = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                              "/.dearsql/custom_shader.glsl";
    std::string path = appState->getSetting("custom_shader_path", defaultPath);
    std::strncpy(shaderPath_, path.c_str(), sizeof(shaderPath_) - 1);
    shaderPath_[sizeof(shaderPath_) - 1] = '\0';

    std::ifstream f(path, std::ios::binary);
    if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        shaderEditor_->SetText(ss.str());
    } else {
        // default to the sample shader (assets/shaders/sample.glsl)
        shaderEditor_->SetText(
            "void mainImage(out vec4 fragColor, in vec2 fragCoord) {\n"
            "    vec2 uv = fragCoord / iResolution.xy;\n"
            "\n"
            "    float wave = sin(uv.y * 40.0 + iTime * 2.0) * 0.0015;\n"
            "\n"
            "    float r = texture(iChannel0, uv + vec2(wave + 0.0010, 0.0)).r;\n"
            "    float g = texture(iChannel0, uv + vec2(wave, 0.0)).g;\n"
            "    float b = texture(iChannel0, uv + vec2(wave - 0.0010, 0.0)).b;\n"
            "    float a = texture(iChannel0, uv).a;\n"
            "\n"
            "    vec3 col = vec3(r, g, b);\n"
            "\n"
            "    col *= 0.92 + 0.08 * sin(uv.y * iResolution.y * 1.5 - iTime * 6.0);\n"
            "\n"
            "    fragColor = vec4(col, a);\n"
            "}\n");
    }
    shaderStatus_ = CustomShader::isLoaded() ? ("Active: " + CustomShader::loadedPath()) : "Off";
}
#endif

void SettingsDialog::render() {
    if (!open_) {
        return;
    }
    if (pendingOpen_) {
        ImGui::OpenPopup(POPUP_ID);
        pendingOpen_ = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    center.y -= 10.0f;
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);

    auto& app = Application::getInstance();
    const auto& colors = app.getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::L, Theme::Spacing::L));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface2);
    if (!ImGui::BeginPopupModal(POPUP_ID, &open_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Theme::Spacing::M, Theme::Spacing::M));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Theme::Spacing::M, Theme::Spacing::S));
    // colored buttons
    ImGui::PushStyleColor(ImGuiCol_Button, colors.surface1);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface2);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.overlay0);

    // --- Appearance ---
    ImGui::SeparatorText("Appearance");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Theme");
    ImGui::SameLine();
    bool dark = app.isDarkTheme();
    if (ImGui::RadioButton("Light", !dark)) {
        app.setDarkTheme(false);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Dark", dark)) {
        app.setDarkTheme(true);
    }

    float scale = app.getFontScale();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Font Size");
    ImGui::SameLine();
    if (UIUtils::Button("A-")) {
        app.setFontScale(scale - 0.1f);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d%%", static_cast<int>(scale * 100));
    ImGui::SameLine();
    if (UIUtils::Button("A+")) {
        app.setFontScale(scale + 0.1f);
    }

#if defined(__APPLE__)
    // --- Custom Shader (macOS / Metal) ---
    ImGui::SeparatorText("Custom Shader");
    ImGui::TextDisabled("Shadertoy-style GLSL run over the whole UI (status: %s)",
                        shaderStatus_.c_str());

    const std::string& shaderErr = CustomShader::lastError();
    if (!shaderErr.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("%s", shaderErr.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("File");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 92.0f);
    ImGui::InputText("##shader_path", shaderPath_, sizeof(shaderPath_));
    ImGui::SameLine();
    if (UIUtils::Button("Browse...", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
        std::string picked = FileDialog::openFile();
        if (!picked.empty()) {
            std::strncpy(shaderPath_, picked.c_str(), sizeof(shaderPath_) - 1);
            shaderPath_[sizeof(shaderPath_) - 1] = '\0';
            std::ifstream f(picked, std::ios::binary);
            if (f && shaderEditor_) {
                std::ostringstream ss;
                ss << f.rdbuf();
                shaderEditor_->SetText(ss.str());
            }
        }
    }

    if (shaderEditor_) {
        shaderEditor_->Render("##shader_code", ImVec2(-1, 240), true);
    }

    bool applyClicked = UIUtils::Button("Apply", UIUtils::ButtonVariant::Primary);
    if (applyClicked) {
        std::string path = shaderPath_;
        if (path.empty()) {
            path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                   "/.dearsql/custom_shader.glsl";
            std::strncpy(shaderPath_, path.c_str(), sizeof(shaderPath_) - 1);
            shaderPath_[sizeof(shaderPath_) - 1] = '\0';
        }
        std::string code = shaderEditor_ ? shaderEditor_->GetText() : std::string();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out << code;
        out.close();
        app.getAppState()->setSetting("custom_shader_path", path);
        app.getAppState()->setSetting("custom_shader_enabled", "1");
        shaderStatus_ = CustomShader::loadFromFile(path) ? ("Active: " + path) : "Compile error";
    }
    ImGui::SameLine();
    if (UIUtils::Button("Disable", UIUtils::ButtonVariant::Danger)) {
        CustomShader::unload();
        // keep custom_shader_path so the shader can be re-enabled later
        app.getAppState()->setSetting("custom_shader_enabled", "0");
        shaderStatus_ = "Off";
    }
#endif

    // --- Actions ---
    ImGui::SeparatorText("About");
    if (onManageLicense && UIUtils::Button("Manage License...")) {
        onManageLicense();
    }
    ImGui::SameLine();
    if (UIUtils::Button("Check for Updates...")) {
        checkForUpdates();
    }
    ImGui::SameLine();
    if (onReportBug && UIUtils::Button("Report Bug...")) {
        onReportBug();
    }

    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));
    if (UIUtils::Button("Close", UIUtils::ButtonVariant::Secondary, ImVec2(-1, 0))) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleColor(3); // button colors
    ImGui::PopStyleVar(2);   // ItemSpacing, FramePadding
    ImGui::EndPopup();
    ImGui::PopStyleColor(); // Border
    ImGui::PopStyleVar(2);  // WindowPadding, WindowBorderSize
}
