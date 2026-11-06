#pragma once

#include <functional>
#include <string>

namespace MasterSecret {
    // per-install secret that encrypts saved credentials, created on first get().
    // stored in a 0600 file (~/.dearsql/master.key) unless the user opts into the OS
    // keystore: macOS Keychain, Windows Credential Manager, Linux libsecret.
    // empty string if it can't be read or stored (caller falls back to legacy key)
    const std::string& get();

    bool keychainSupported();
    bool keyFileExists();
    // where get() looks; set before the first get()
    void useKeychain(bool on);
    bool usingKeychain();
    // moves the secret between the keystore and the file: writes the new copy, then
    // saveSetting (undone if it fails), then deletes the old copy. empty on success,
    // else what went wrong
    std::string moveToKeychain(bool on, const std::function<bool()>& saveSetting);
} // namespace MasterSecret
