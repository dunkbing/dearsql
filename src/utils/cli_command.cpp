#include "utils/cli_command.hpp"

#include "utils/process_runner.hpp"
#include <cstdlib>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace {

    // the file the link should point at; for an AppImage, the image itself rather
    // than its temporary mount
    fs::path target() {
#if defined(__APPLE__)
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) != 0)
            return {};
        std::error_code ec;
        auto p = fs::canonical(buf, ec);
        return ec ? fs::path(buf) : p;
#elif defined(__linux__)
        if (const char* image = std::getenv("APPIMAGE"); image && *image)
            return image;
        std::error_code ec;
        auto p = fs::canonical("/proc/self/exe", ec);
        return ec ? fs::path{} : p;
#else
        return {};
#endif
    }

    std::string tryLink(const fs::path& link, const fs::path& to) {
        std::error_code ec;
        fs::create_directories(link.parent_path(), ec);
        if (fs::is_symlink(link, ec) || fs::exists(link, ec))
            fs::remove(link, ec);
        if (ec)
            return ec.message();
        fs::create_symlink(to, link, ec);
        return ec ? ec.message() : "";
    }

#if defined(__APPLE__)
    // a path inside a single-quoted sh word inside an AppleScript string
    std::string shellQuote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += "'";
        std::string escaped;
        for (char c : out) {
            if (c == '"' || c == '\\')
                escaped += '\\';
            escaped += c;
        }
        return escaped;
    }
#endif

} // namespace

namespace CliCommand {

    bool supported() {
#if defined(__APPLE__) || defined(__linux__)
        return true;
#else
        return false;
#endif
    }

    std::string linkPath() {
        if (const char* custom = std::getenv("DEARSQL_CLI_LINK"); custom && *custom)
            return custom;
#if defined(__APPLE__)
        return "/usr/local/bin/dearsql";
#elif defined(__linux__)
        const char* home = std::getenv("HOME");
        return (fs::path(home ? home : ".") / ".local/bin/dearsql").string();
#else
        return "";
#endif
    }

    bool installed() {
        std::error_code ec;
        const fs::path link = linkPath();
        if (link.empty() || !fs::is_symlink(link, ec))
            return false;
        auto points = fs::canonical(link, ec);
        return !ec && points == target();
    }

    std::string install() {
        const fs::path to = target();
        const fs::path link = linkPath();
        if (to.empty() || link.empty())
            return "not supported on this platform";
        auto err = tryLink(link, to);
        if (err.empty())
            return "";
#if defined(__APPLE__)
        // /usr/local/bin is root-owned on a fresh Apple Silicon Mac: ask once, as
        // `code` and similar tools do
        const std::string script = "do shell script \"mkdir -p /usr/local/bin && ln -sf " +
                                   shellQuote(to.string()) + " " + shellQuote(link.string()) +
                                   "\" with administrator privileges";
        auto r = ProcessRunner::run({.args = {"/usr/bin/osascript", "-e", script}});
        if (r.success && installed())
            return "";
        spdlog::warn("installing the dearsql command failed: {} {}", r.errorMessage, r.output);
        return r.output.find("User canceled") != std::string::npos
                   ? "cancelled"
                   : "could not write " + link.string();
#else
        return err;
#endif
    }

} // namespace CliCommand
