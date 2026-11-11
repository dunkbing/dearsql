#pragma once

// the app's ui font: the embedded ones (monospace text, icons, cyrillic) plus a
// system CJK font when one is installed (imshell::AddFont)
namespace fonts {

    // load it into ImGui's atlas; once, after the ImGui context exists
    void setup();

} // namespace fonts
