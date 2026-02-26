#pragma once

#if defined(__linux__)

#include <string>

/// Initialize the Linux AppImage updater. Starts a silent background version check.
/// No-op when not running as an AppImage ($APPIMAGE unset).
void initializeLinuxUpdater();

/// Manually check for updates (triggered from menu button).
void checkForUpdatesLinux();

/// Poll async operations and update the UI dialog. Call once per frame.
void pollLinuxUpdater();

/// Returns true if a newer version was detected by the background check.
bool isLinuxUpdateAvailable();

/// Returns the latest version string (empty if no update available).
std::string getLinuxLatestVersion();

#endif
