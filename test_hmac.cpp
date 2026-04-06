#include <iostream>
#include <openssl/hmac.h>
#include <string>

int main() {
    std::string key = "my_secret_key";
    std::string data = "my_data";
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len;
    HMAC(EVP_sha256(), key.c_str(), key.length(), (const unsigned char*)data.c_str(), data.length(), result, &result_len);
    std::cout << "HMAC success\n";
    return 0;
}
