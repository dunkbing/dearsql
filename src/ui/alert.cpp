#include "ui/alert.hpp"

#include "application.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "mydear/button.hpp"
#include "themes.hpp"
#include "ui/input_dialog.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <variant>

namespace {
    struct Message {
        std::string title, message;
        std::vector<AlertButton> buttons;
    };
    struct Prompt {
        std::string title, label, confirmText;
        InputDialog::ConfirmCallback onConfirm;
        InputDialog::CancelCallback onCancel;
        InputDialog::ValidatorCallback validator;
        char value[512] = {};
        std::string error;
    };
    using Entry = std::variant<Message, Prompt>;

    // pushed from any thread, drawn and popped on the ui thread. deque push_back keeps
    // references to the front valid, so it is used unlocked while drawn
    std::mutex gMutex;
    std::deque<Entry> gQueue;
    bool gOpen = false; // the front entry's popup was opened

    constexpr const char* kPopup = "###dearsql_alert";

    void push(Entry entry) {
        std::lock_guard lock(gMutex);
        gQueue.push_back(std::move(entry));
    }

    float buttonWidth(const std::string& text) {
        return std::max(80.0f,
                        ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2);
    }

    // a right-aligned row; the index of the pressed button, or -1
    int buttonRow(const std::vector<std::pair<std::string, mydear::ButtonVariant>>& buttons) {
        float total = 0;
        for (const auto& [text, _] : buttons)
            total += buttonWidth(text) + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x -
                                                                  total +
                                                                  ImGui::GetStyle().ItemSpacing.x));
        int pressed = -1;
        for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
            if (i)
                ImGui::SameLine();
            if (mydear::Button(buttons[i].first.c_str(), buttons[i].second,
                               ImVec2(buttonWidth(buttons[i].first), 0)))
                pressed = i;
        }
        return pressed;
    }

    // not on the frame it appears: the key that opened it would answer it
    bool escape() {
        return !ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    }
    bool enter() {
        return !ImGui::IsWindowAppearing() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    }

    // the callback to run once the popup is closed, or nothing while it stays open
    std::function<void()> drawMessage(Message& m) {
        ImGui::PushTextWrapPos(480.0f);
        if (!m.message.empty())
            ImGui::TextUnformatted(m.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // cancel on the left, the default action on the right (return runs it)
        std::vector<size_t> order;
        for (auto style : {AlertButton::Style::Cancel, AlertButton::Style::Destructive,
                           AlertButton::Style::Default})
            for (size_t i = 0; i < m.buttons.size(); ++i)
                if (m.buttons[i].style == style)
                    order.push_back(i);
        std::vector<std::pair<std::string, mydear::ButtonVariant>> row;
        for (size_t i : order) {
            const auto style = m.buttons[i].style;
            row.emplace_back(m.buttons[i].text, style == AlertButton::Style::Default
                                                    ? mydear::ButtonVariant::Primary
                                                : style == AlertButton::Style::Destructive
                                                    ? mydear::ButtonVariant::Danger
                                                    : mydear::ButtonVariant::Secondary);
        }
        int pressed = buttonRow(row);
        if (pressed < 0) {
            // esc: the cancel button, or the only one; return: the default one, never a
            // destructive one
            const bool single = order.size() == 1;
            const auto first = m.buttons[order.front()].style;
            const auto last = m.buttons[order.back()].style;
            if (escape() && (first == AlertButton::Style::Cancel || single))
                pressed = 0;
            else if (enter() && last == AlertButton::Style::Default)
                pressed = static_cast<int>(order.size()) - 1;
        }
        if (pressed < 0)
            return nullptr;
        auto onPress = m.buttons[order[pressed]].onPress;
        return onPress ? onPress : [] {};
    }

    std::function<void()> drawPrompt(Prompt& p) {
        if (!p.label.empty()) {
            ImGui::PushTextWrapPos(480.0f);
            ImGui::TextUnformatted(p.label.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(std::max(360.0f, ImGui::GetContentRegionAvail().x));
        const bool submitted = ImGui::InputText("##value", p.value, sizeof(p.value),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (!p.error.empty()) {
            ImGui::PushTextWrapPos(480.0f);
            ImGui::TextColored(Application::getInstance().getCurrentColors().red, "%s",
                               p.error.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        const int pressed = buttonRow({{"Cancel", mydear::ButtonVariant::Secondary},
                                       {p.confirmText, mydear::ButtonVariant::Primary}});
        if (pressed == 0 || (escape() && pressed < 0))
            return p.onCancel ? p.onCancel : [] {};
        if (pressed != 1 && !submitted)
            return nullptr;
        // a validator or confirm error keeps the prompt open, with the message
        const std::string value = p.value;
        p.error = p.validator ? p.validator(value) : "";
        if (p.error.empty() && p.onConfirm)
            p.error = p.onConfirm(value);
        if (!p.error.empty())
            return nullptr;
        return [] {};
    }
} // namespace

void Alert::show(const std::string& title, const std::string& message,
                 std::vector<AlertButton> buttons) {
    if (buttons.empty())
        buttons.push_back({"OK", nullptr, AlertButton::Style::Default});
    push(Message{title, message, std::move(buttons)});
}

void InputDialog::show(const std::string& title, const std::string& label,
                       const std::string& initialValue, const std::string& confirmButtonText,
                       ConfirmCallback onConfirm, CancelCallback onCancel,
                       ValidatorCallback validator) {
    Prompt p{title,
             label,
             confirmButtonText,
             std::move(onConfirm),
             std::move(onCancel),
             std::move(validator)};
    std::snprintf(p.value, sizeof(p.value), "%s", initialValue.c_str());
    push(std::move(p));
}

void Alert::render() {
    Entry* entry = nullptr;
    {
        std::lock_guard lock(gMutex);
        if (gQueue.empty())
            return;
        entry = &gQueue.front();
    }

    // re-enter the popups drawn this frame (settings, a dialog), so the alert stacks on top
    // of them: opened from the root it would replace them
    ImGuiContext& g = *ImGui::GetCurrentContext();
    int depth = 0;
    while (depth < g.OpenPopupStack.Size) {
        ImGuiWindow* w = g.OpenPopupStack[depth].Window;
        if (!w || !w->Active || std::strstr(w->Name, kPopup))
            break;
        ImGui::Begin(w->Name, nullptr, w->Flags);
        ++depth;
    }

    const std::string& title =
        std::visit([](auto& e) -> const std::string& { return e.title; }, *entry);
    const std::string label = title + kPopup;
    if (!gOpen) {
        ImGui::OpenPopup(kPopup);
        gOpen = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(560.0f, FLT_MAX));
    std::function<void()> done;
    bool closed = false;
    if (ImGui::BeginPopupModal(label.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(Theme::Spacing::M, Theme::Spacing::M));
        done = std::visit(
            [](auto& e) {
                if constexpr (std::is_same_v<std::decay_t<decltype(e)>, Message>)
                    return drawMessage(e);
                else
                    return drawPrompt(e);
            },
            *entry);
        ImGui::PopStyleVar();
        if (done) {
            ImGui::CloseCurrentPopup();
            closed = true;
        }
        ImGui::EndPopup();
    } else {
        // closed from outside (the popup under it went away): a prompt counts as cancelled
        closed = true;
        if (auto* p = std::get_if<Prompt>(entry); p && p->onCancel)
            done = p->onCancel;
    }
    for (; depth > 0; --depth)
        ImGui::EndPopup();

    if (closed) {
        gOpen = false;
        {
            std::lock_guard lock(gMutex);
            gQueue.pop_front();
        }
        if (done)
            done(); // last: it may show another alert
    }
}
