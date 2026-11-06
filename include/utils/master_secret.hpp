#pragma once

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
    // moves the secret between the keystore and the file; empty on success, else why not
    std::string moveToKeychain(bool on);
} // namespace MasterSecret
