#pragma once

class Application;

// the app's bar inside imshell's title bar strip: sidebar toggle, new connection, title,
// workspace picker, settings. every desktop
namespace TitleBarView {
    void render(Application& app);
}
