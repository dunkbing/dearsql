#include "utils/master_secret.hpp"
#include "utils/crypto.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        if (const char* override_ = std::getenv("DEARSQL_MASTER_KEY_FILE")) {
            return override_;
        }
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

    // 0600 file fallback for keystore-less environments
    std::string loadFromFile() {
        const fs::path path = keyFilePath();
        if (path.empty()) {
            return "";
        }
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        if (std::ifstream in(path); in) {
            std::string secret;
            std::getline(in, secret);
            if (!secret.empty()) {
                return secret;
            }
        }

        // create empty + chmod before writing the secret
        {
            std::ofstream create(path, std::ios::trunc);
            if (!create) {
                spdlog::warn("cannot create master key file {}", path.string());
                return "";
            }
        }
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, ec);

        const std::string secret = newSecret();
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            return "";
        }
        out << secret << "\n";
        return secret;
    }

#if defined(__APPLE__)

    std::string loadFromKeychain() {
        CFStringRef service = CFSTR("DearSQL");
        CFStringRef account = CFSTR("master-secret");

        const void* qKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData,
                               kSecMatchLimit};
        const void* qValues[] = {kSecClassGenericPassword, service, account, kCFBooleanTrue,
                                 kSecMatchLimitOne};
        CFDictionaryRef query =
            CFDictionaryCreate(nullptr, qKeys, qValues, 5, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        CFTypeRef result = nullptr;
        OSStatus status = SecItemCopyMatching(query, &result);
        CFRelease(query);

        if (status == errSecSuccess && result) {
            const auto data = static_cast<CFDataRef>(result);
            std::string secret(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                               static_cast<size_t>(CFDataGetLength(data)));
            CFRelease(result);
            if (!secret.empty()) {
                return secret;
            }
        } else if (status != errSecItemNotFound) {
            spdlog::warn("keychain read failed (status {}), keeping legacy credential key",
                         static_cast<int>(status));
            return "";
        }

        const std::string secret = newSecret();
        CFDataRef secretData =
            CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()), secret.size());
        const void* aKeys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
        const void* aValues[] = {kSecClassGenericPassword, service, account, secretData};
        CFDictionaryRef attrs =
            CFDictionaryCreate(nullptr, aKeys, aValues, 4, &kCFTypeDictionaryKeyCallBacks,
                               &kCFTypeDictionaryValueCallBacks);
        status = SecItemAdd(attrs, nullptr);
        CFRelease(attrs);
        CFRelease(secretData);
        if (status != errSecSuccess) {
            spdlog::warn("keychain write failed (status {}), keeping legacy credential key",
                         static_cast<int>(status));
            return "";
        }
        return secret;
    }

#elif defined(_WIN32)

    std::string loadFromCredentialManager() {
        const wchar_t* target = L"DearSQL/master-secret";
        PCREDENTIALW cred = nullptr;
        if (CredReadW(target, CRED_TYPE_GENERIC, 0, &cred)) {
            std::string secret(reinterpret_cast<char*>(cred->CredentialBlob),
                               cred->CredentialBlobSize);
            CredFree(cred);
            if (!secret.empty()) {
                return secret;
            }
        }

        const std::string secret = newSecret();
        CREDENTIALW newCred = {};
        newCred.Type = CRED_TYPE_GENERIC;
        newCred.TargetName = const_cast<LPWSTR>(target);
        newCred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
        newCred.CredentialBlobSize = static_cast<DWORD>(secret.size());
        newCred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&newCred, 0)) {
            spdlog::warn("credential manager write failed ({}), keeping legacy credential key",
                         GetLastError());
            return "";
        }
        return secret;
    }

#elif defined(HAVE_LIBSECRET)

    std::string loadFromSecretService() {
        static const SecretSchema schema = {"com.dearsql.master-secret",
                                            SECRET_SCHEMA_NONE,
                                            {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                             {nullptr, static_cast<SecretSchemaAttributeType>(0)}}};

        GError* err = nullptr;
        gchar* found = secret_password_lookup_sync(&schema, nullptr, &err, "account",
                                                   "master-secret", nullptr);
        if (err) {
            g_error_free(err);
            return "";
        }
        if (found) {
            std::string secret(found);
            secret_password_free(found);
            if (!secret.empty()) {
                return secret;
            }
        }

        const std::string secret = newSecret();
        const gboolean ok = secret_password_store_sync(
            &schema, SECRET_COLLECTION_DEFAULT, "DearSQL master secret", secret.c_str(), nullptr,
            &err, "account", "master-secret", nullptr);
        if (err) {
            g_error_free(err);
            return "";
        }
        return ok ? secret : "";
    }

#endif

    std::string load() {
        try {
            if (std::getenv("DEARSQL_MASTER_KEY_FILE")) {
                return loadFromFile();
            }
#if defined(__APPLE__)
            return loadFromKeychain();
#elif defined(_WIN32)
            return loadFromCredentialManager();
#else
#ifdef HAVE_LIBSECRET
            if (std::string secret = loadFromSecretService(); !secret.empty()) {
                return secret;
            }
            spdlog::warn("secret service unavailable, storing master key in ~/.dearsql/master.key");
#else
            spdlog::warn("built without libsecret, storing master key in ~/.dearsql/master.key");
#endif
            return loadFromFile();
#endif
        } catch (const std::exception& e) {
            spdlog::warn("master secret unavailable ({}), keeping legacy credential key", e.what());
            return "";
        }
    }

} // namespace

namespace MasterSecret {
    const std::string& get() {
        // ponytail: single keystore lookup per run; if the backend that created the
        // secret later disappears (e.g. secret service removed), affected rows fail
        // decryption and are rewritten on next save — no dual-backend lookup.
        static const std::string secret = load();
        return secret;
    }
} // namespace MasterSecret
