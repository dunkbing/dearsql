#pragma once

struct ImFont;

// the app's fonts: the embedded ones (monospace text, icons, cyrillic) plus a
// system CJK font when one is installed
namespace fonts {

    // load them into ImGui's atlas; once, after the ImGui context exists
    void setup();

    // monospace + CJK for tab content (sql editor, table viewer); null without a
    // system CJK font, in which case the default font is used
    ImFont* tabFont();

} // namespace fonts
