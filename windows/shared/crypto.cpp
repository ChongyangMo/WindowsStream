#include "../src/common.h"
#include "crypto.h"
#include <bcrypt.h>

bool HmacSha256(const std::string& key, const uint8_t* data, size_t size, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0) {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
                            static_cast<ULONG>(key.size()), 0) >= 0) {
            ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0 &&
                 BCryptFinishHash(hash, out, 32, 0) >= 0;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool SecureEqual(const uint8_t* left, const uint8_t* right, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}
