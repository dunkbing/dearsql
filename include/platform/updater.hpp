#pragma once

#include <string>

/// initialize the platform updater. starts a silent background version check.
void initializeUpdater();

/// manually check for updates (triggered from menu button).
void checkForUpdates();

/// poll async operations and update the UI dialog. call once per frame.
/// no-op on macOS/Windows (Sparkle/WinSparkle handle their own UI).
void pollUpdater();

/// clean up updater resources. no-op on macOS/Linux.
void cleanupUpdater();

/// returns true if a newer version was detected by the background check.
/// only meaningful on Linux; returns false on macOS/Windows.
bool isUpdateAvailable();

/// returns the latest version string (empty if no update available).
/// only meaningful on Linux; returns empty on macOS/Windows.
std::string getLatestVersion();
