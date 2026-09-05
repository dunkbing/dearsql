#pragma once

#include <string>

namespace MasterSecret {
    // per-install secret from the OS keystore, created on first call.
    // empty string if the keystore is unavailable (caller falls back to legacy key).
    // backends: macOS Keychain, Windows Credential Manager, Linux libsecret
    // with a 0600 file fallback (~/.dearsql/master.key).
    // debug builds always use the file (no keychain prompt per rebuild).
    const std::string& get();
} // namespace MasterSecret
