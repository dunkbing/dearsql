#pragma once

class Application;

// the app's bar inside mydear's title bar strip: sidebar toggle, new connection, title,
// workspace picker, settings. macos now; windows and linux once their shells move to mydear
namespace TitleBarView {
    void render(Application& app);
}
