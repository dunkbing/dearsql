#pragma once

#include "mydear/app.hpp"

// tag of the imgui view (the file picker pauses it)
inline constexpr long kDearSQLViewTag = 0xDEA5;

// ipad chrome around mydear's uikit view: the navigation bar, whose items live on
// DearSQLViewController, and the DEARSQL_SHOW screenshot hooks
namespace ios {
    void attach(mydear::App& shell); // tags the view
    void renderChrome();             // DEARSQL_SHOW
    void applyTheme(bool dark);      // the navigation bar
} // namespace ios
