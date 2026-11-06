#include "utils/master_secret.hpp"
#include "utils/crypto.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <spdlog/spdlog.h>
#include <vector>

#if defined(__APPLE__)
#include <Security/Security.h>
#elif defined(_WIN32)
#include <windows.h>
// clang-format off
#include <wincred.h>
// clang-format on
#elif defined(HAVE_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace fs = std::filesystem;

namespace {

    std::string newSecret() {
        const std::string key = CryptoUtils::generateKey();
        return CryptoUtils::base64Encode(std::vector<uint8_t>(key.begin(), key.end()));
    }

    fs::path keyFilePath() {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (!home) {
            return {};
        }
        return fs::path(home) / ".dearsql" / "master.key";
    }

    std::string readFile() {
        std::string secret;
        if (std::ifstream in(keyFilePath()); in) {
            std::getline(in, secret);
        }
        return secret;
    }

    bool writeFile(const std::string& secret) {
        const fs::path path = keyFilePath();
        if (path.empty()) {
            return false;
        }
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        // create empty + chmod before writing the secret
        {
            std::ofstream create(path, std::ios::trunc);
            if (!create) {
                spdlog::warn("cannot create master key file {}", path.string());
                return false;
            }
        }
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, ec);
        std::ofstream out(path, std::ios::trunc);
        out << secret << "\n";
        return static_cast<bool>(out.flush());
    }

    // keystore: nullopt = unavailable, "" = no entry yet
#if defined(__APPLE__)

    CFDictionaryRef keychainQuery(bool returnData) {
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                              kSecMatchLimit};
        const void* values[] = {kSecClassGenericPassword, CFSTR("DearSQL"), CFSTR("master-secret"),
                                kCFBooleanTrue, kSecMatchLimitOne};
        return CFDictionaryCreate(nullptr, keys, values, returnData ? 5 : 3,
                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    std::optional<std::string> keystoreRead() {
        CFDictionaryRef query = keychainQuery(true);
        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &result);
        CFRelease(query);
        if (status == errSecItemNotFound) {
            return "";
        }
        if (status != errSecSuccess || !result) {
            spdlog::warn("keychain read failed (status {})", static_cast<int>(status));
            return std::nullopt;
        }
        const auto data = static_cast<CFDataRef>(result);
        std::string secret(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                           static_cast<size_t>(CFDataGetLength(data)));
        CFRelease(result);
        return secret;
    }

    void keystoreDelete() {
        CFDictionaryRef query = keychainQuery(false);
        SecItemDelete(query);
        CFRelease(query);
    }

    bool keystoreWrite(const std::string& secret) {
        keystoreDelete();
        CFDataRef secretData =
            CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()), secret.size());
        const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
        const void* values[] = {kSecClassGenericPassword, CFSTR("DearSQL"), CFSTR("master-secret"),
                                secretData};
        CFDictionaryRef attrs =
            CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        const OSStatus status = SecItemAdd(attrs, nullptr);
        CFRelease(attrs);
        CFRelease(secretData);
        if (status != errSecSuccess) {
            spdlog::warn("keychain write failed (status {})", static_cast<int>(status));
        }
        return status == errSecSuccess;
    }

#elif defined(_WIN32)

    constexpr const wchar_t* kTarget = L"DearSQL/master-secret";

    std::optional<std::string> keystoreRead() {
        PCREDENTIALW cred = nullptr;
        if (!CredReadW(kTarget, CRED_TYPE_GENERIC, 0, &cred)) {
            return GetLastError() == ERROR_NOT_FOUND ? std::optional<std::string>("")
                                                     : std::nullopt;
        }
        std::string secret(reinterpret_cast<char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        CredFree(cred);
        return secret;
    }

    bool keystoreWrite(const std::string& secret) {
        CREDENTIALW cred = {};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = const_cast<LPWSTR>(kTarget);
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&cred, 0)) {
            spdlog::warn("credential manager write failed ({})", GetLastError());
            return false;
        }
        return true;
    }

    void keystoreDelete() {
        CredDeleteW(kTarget, CRED_TYPE_GENERIC, 0);
    }

#elif defined(HAVE_LIBSECRET)

    const SecretSchema* schema() {
        static const SecretSchema s = {"com.dearsql.master-secret",
                                       SECRET_SCHEMA_NONE,
                                       {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                        {nullptr, static_cast<SecretSchemaAttributeType>(0)}}};
        return &s;
    }

    std::optional<std::string> keystoreRead() {
        GError* err = nullptr;
        gchar* found = secret_password_lookup_sync(schema(), nullptr, &err, "account",
                                                   "master-secret", nullptr);
        if (err) {
            g_error_free(err);
            return std::nullopt;
        }
        std::string secret = found ? found : "";
        if (found) {
            secret_password_free(found);
        }
        return secret;
    }

    bool keystoreWrite(const std::string& secret) {
        GError* err = nullptr;
        const gboolean ok = secret_password_store_sync(
            schema(), SECRET_COLLECTION_DEFAULT, "DearSQL master secret", secret.c_str(), nullptr,
            &err, "account", "master-secret", nullptr);
        if (err) {
            g_error_free(err);
            return false;
        }
        return ok;
    }

    void keystoreDelete() {
        secret_password_clear_sync(schema(), nullptr, nullptr, "account", "master-secret", nullptr);
    }

#else

    std::optional<std::string> keystoreRead() {
        return std::nullopt;
    }
    bool keystoreWrite(const std::string&) {
        return false;
    }
    void keystoreDelete() {}

#endif

    bool gUseKeychain = false;

    std::string load() {
        try {
            if (gUseKeychain) {
                const std::optional<std::string> found = keystoreRead();
                if (!found) {
                    return "";
                }
                if (!found->empty()) {
                    return *found;
                }
                std::string secret = newSecret();
                return keystoreWrite(secret) ? secret : "";
            }
            if (std::string secret = readFile(); !secret.empty()) {
                return secret;
            }
            std::string secret = newSecret();
            return writeFile(secret) ? secret : "";
        } catch (const std::exception& e) {
            spdlog::warn("master secret unavailable ({}), keeping legacy credential key", e.what());
            return "";
        }
    }

} // namespace

namespace MasterSecret {
    const std::string& get() {
        // the value never changes for a run; moveToKeychain only moves where it lives
        static const std::string secret = load();
        return secret;
    }

    bool keychainSupported() {
#if defined(__APPLE__) || defined(_WIN32) || defined(HAVE_LIBSECRET)
        return true;
#else
        return false;
#endif
    }

    bool keyFileExists() {
        std::error_code ec;
        return fs::exists(keyFilePath(), ec);
    }

    void useKeychain(bool on) {
        gUseKeychain = on && keychainSupported();
    }

    bool usingKeychain() {
        return gUseKeychain;
    }

    std::string moveToKeychain(bool on) {
        if (on == gUseKeychain) {
            return "";
        }
        const std::string& secret = get();
        if (secret.empty()) {
            return "no encryption key to move";
        }
        if (on) {
            if (!keystoreWrite(secret)) {
                return "could not write to the keychain";
            }
            std::error_code ec;
            fs::remove(keyFilePath(), ec);
        } else {
            if (!writeFile(secret)) {
                return "could not write " + keyFilePath().string();
            }
            keystoreDelete();
        }
        gUseKeychain = on;
        return "";
    }
} // namespace MasterSecret
