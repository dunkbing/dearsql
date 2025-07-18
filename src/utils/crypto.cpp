#include "utils/crypto.hpp"
#include <cstring>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <random>
#include <stdexcept>

namespace CryptoUtils {

    std::string encrypt(const std::string &plaintext, const std::string &key) {
        if (key.length() != 32) {
            throw std::invalid_argument("Key must be 32 bytes for AES-256");
        }

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            throw std::runtime_error("Failed to create cipher context");
        }

        // Generate random IV
        unsigned char iv[12]; // 96-bit IV for GCM
        if (RAND_bytes(iv, sizeof(iv)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to generate IV");
        }

        // Initialize encryption
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to initialize encryption");
        }

        // Set IV length
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to set IV length");
        }

        // Set key and IV
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.c_str()), iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to set key and IV");
        }

        // Encrypt the plaintext
        std::vector<unsigned char> ciphertext(plaintext.length() +
                                              EVP_CIPHER_block_size(EVP_aes_256_gcm()));
        int len;
        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                              reinterpret_cast<const unsigned char *>(plaintext.c_str()),
                              plaintext.length()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to encrypt");
        }

        int ciphertext_len = len;

        // Finalize encryption
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to finalize encryption");
        }
        ciphertext_len += len;

        // Get the tag
        unsigned char tag[16];
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to get authentication tag");
        }

        EVP_CIPHER_CTX_free(ctx);

        // Combine IV + ciphertext + tag
        std::vector<uint8_t> result;
        result.insert(result.end(), iv, iv + sizeof(iv));
        result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);
        result.insert(result.end(), tag, tag + sizeof(tag));

        return base64Encode(result);
    }

    std::string decrypt(const std::string &ciphertext, const std::string &key) {
        if (key.length() != 32) {
            throw std::invalid_argument("Key must be 32 bytes for AES-256");
        }

        auto data = base64Decode(ciphertext);

        if (data.size() < 12 + 16) { // IV + tag minimum
            throw std::invalid_argument("Invalid ciphertext format");
        }

        // Extract IV, ciphertext, and tag
        unsigned char iv[12];
        std::memcpy(iv, data.data(), 12);

        unsigned char tag[16];
        std::memcpy(tag, data.data() + data.size() - 16, 16);

        size_t cipher_len = data.size() - 12 - 16;

        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx) {
            throw std::runtime_error("Failed to create cipher context");
        }

        // Initialize decryption
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to initialize decryption");
        }

        // Set IV length
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), nullptr) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to set IV length");
        }

        // Set key and IV
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                               reinterpret_cast<const unsigned char *>(key.c_str()), iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to set key and IV");
        }

        // Decrypt the ciphertext
        std::vector<unsigned char> plaintext(cipher_len);
        int len;
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, data.data() + 12, cipher_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to decrypt");
        }

        int plaintext_len = len;

        // Set the tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, sizeof(tag), tag) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Failed to set authentication tag");
        }

        // Finalize decryption
        if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw std::runtime_error("Decryption failed - authentication error");
        }
        plaintext_len += len;

        EVP_CIPHER_CTX_free(ctx);

        return std::string(plaintext.begin(), plaintext.begin() + plaintext_len);
    }

    std::string generateKey() {
        unsigned char key[32];
        if (RAND_bytes(key, sizeof(key)) != 1) {
            throw std::runtime_error("Failed to generate random key");
        }
        return std::string(reinterpret_cast<char *>(key), sizeof(key));
    }

    std::string deriveKey(const std::string &password, const std::string &salt) {
        unsigned char key[32];

        if (PKCS5_PBKDF2_HMAC(password.c_str(), password.length(),
                              reinterpret_cast<const unsigned char *>(salt.c_str()), salt.length(),
                              100000, // iterations
                              EVP_sha256(), sizeof(key), key) != 1) {
            throw std::runtime_error("Failed to derive key from password");
        }

        return std::string(reinterpret_cast<char *>(key), sizeof(key));
    }

    std::string generateSalt() {
        unsigned char salt[16];
        if (RAND_bytes(salt, sizeof(salt)) != 1) {
            throw std::runtime_error("Failed to generate salt");
        }
        return std::string(reinterpret_cast<char *>(salt), sizeof(salt));
    }

    std::string base64Encode(const std::vector<uint8_t> &data) {
        BIO *bio, *b64;
        BUF_MEM *bufferPtr;

        b64 = BIO_new(BIO_f_base64());
        bio = BIO_new(BIO_s_mem());
        bio = BIO_push(b64, bio);

        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bio, data.data(), data.size());
        BIO_flush(bio);
        BIO_get_mem_ptr(bio, &bufferPtr);

        std::string result(bufferPtr->data, bufferPtr->length);
        BIO_free_all(bio);

        return result;
    }

    std::vector<uint8_t> base64Decode(const std::string &encoded) {
        BIO *bio, *b64;

        b64 = BIO_new(BIO_f_base64());
        bio = BIO_new_mem_buf(encoded.c_str(), encoded.length());
        bio = BIO_push(b64, bio);

        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

        std::vector<uint8_t> result(encoded.length());
        int decoded_length = BIO_read(bio, result.data(), encoded.length());
        BIO_free_all(bio);

        if (decoded_length < 0) {
            throw std::runtime_error("Failed to decode base64");
        }

        result.resize(decoded_length);
        return result;
    }

} // namespace CryptoUtils
