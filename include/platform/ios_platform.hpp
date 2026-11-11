#pragma once

#include "imshell/app.hpp"

// tag of the imgui view (the file picker pauses it)
inline constexpr long kDearSQLViewTag = 0xDEA5;

// ipad chrome around imshell's uikit view: the navigation bar, whose items live on
// DearSQLViewController
namespace ios {
    void attach(imshell::App& shell); // tags the view
    void applyTheme(bool dark);      // the navigation bar
} // namespace ios
