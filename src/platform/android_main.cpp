// android: NativeActivity loads libdearsql.so and the ndk glue calls android_main on its own
// thread; mydear owns the window and the loop from there, as on the desktop
#include "application.hpp"
#include "mydear/android.hpp"

#include <spdlog/sinks/android_sink.h>
#include <spdlog/spdlog.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>

namespace {
    bool start(android_app* state, Application& app) {
        spdlog::set_default_logger(spdlog::android_logger_mt("dearsql", "DearSQL"));
#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);
#endif
        // ~/.dearsql and the documents folder live in the app's private storage
        setenv("HOME", state->activity->internalDataPath, 1);
        if (!app.initialize())
            return false;
        // like `dearsql <file>` on the desktop: adb shell setprop debug.dearsql.open <path>
        char open[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.dearsql.open", open) > 0)
            app.openFile(open);
        return true;
    }
} // namespace

void android_main(android_app* state) {
    mydear::SetAndroidApp(state); // re-attaches the running shell after a recreation
    auto& app = Application::getInstance();
    // the process outlives an activity recreated by a configuration change (system colours,
    // an overlay), so the second android_main finds the app up and carries on where it was
    static bool started = false;
    if (!started) {
        if (!start(state, app))
            std::exit(1);
        started = true;
    }
    app.run();
    if (mydear::AndroidActivityRecreating())
        return; // the next activity's android_main runs it again
    app.cleanup();
    // shortcut: the app's singletons are done for; a relaunch gets a fresh process. _Exit: static
    // destructors racing android's own threads (hwui) abort the process on the way out
    std::fflush(nullptr);
    std::_Exit(0);
}
