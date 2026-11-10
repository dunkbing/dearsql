// android: NativeActivity loads libdearsql.so and the ndk glue calls android_main on its own
// thread; mydear owns the window and the loop from there, as on the desktop
#include "application.hpp"
#include "mydear/android.hpp"

#include <spdlog/sinks/android_sink.h>
#include <spdlog/spdlog.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>

void android_main(android_app* state) {
    mydear::SetAndroidApp(state);
    spdlog::set_default_logger(spdlog::android_logger_mt("dearsql", "DearSQL"));
#ifdef NDEBUG
    spdlog::set_level(spdlog::level::info);
#else
    spdlog::set_level(spdlog::level::debug);
#endif
    // ~/.dearsql and the documents folder live in the app's private storage
    setenv("HOME", state->activity->internalDataPath, 1);

    auto& app = Application::getInstance();
    if (!app.initialize())
        std::exit(1);
    // like `dearsql <file>` on the desktop: adb shell setprop debug.dearsql.open <path>
    char open[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.dearsql.open", open) > 0)
        app.openFile(open);
    app.run();
    app.cleanup();
    // shortcut: the app's singletons are done for; a relaunch gets a fresh process. _Exit: static
    // destructors racing android's own threads (hwui) abort the process on the way out
    std::fflush(nullptr);
    std::_Exit(0);
}
