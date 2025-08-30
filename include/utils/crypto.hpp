#pragma once

#include <string>
#include <vector>

namespace CryptoUtils {
    // AES-256-GCM encryption/decryption
    std::string encrypt(const std::string& plaintext, const std::string& key);
    std::string decrypt(const std::string& ciphertext, const std::string& key);

    // Generate a random encryption key
    std::string generateKey();

    // Derive a key from a password using PBKDF2
    std::string deriveKey(const std::string& password, const std::string& salt);

    // Generate a random salt
    std::string generateSalt();

    // Base64 encoding/decoding for storage
    std::string base64Encode(const std::vector<uint8_t>& data);
    std::vector<uint8_t> base64Decode(const std::string& encoded);
} // namespace CryptoUtils
