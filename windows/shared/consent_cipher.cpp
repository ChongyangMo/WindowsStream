#include "consent_cipher.h"
#include "crypto.h"
#include <cstring>
#include <limits>

namespace {
constexpr ULONG kTagSize = kConsentRecordOverhead - 1;
constexpr size_t kMaxNativeSize = std::numeric_limits<ULONG>::max();

bool Derive(const std::string& token, const std::string& purpose, const uint8_t* hostNonce,
            const uint8_t* viewerNonce, uint8_t out[32]) {
    Writer material;
    material.append(purpose.data(), purpose.size());
    material.append(hostNonce, kConsentNonceSize);
    material.append(viewerNonce, kConsentNonceSize);
    return HmacSha256(token, material.bytes.data(), material.bytes.size(), out);
}

bool MakeKey(BCRYPT_ALG_HANDLE algorithm, const std::string& token, const char* purpose,
             const uint8_t* hostNonce, const uint8_t* viewerNonce, BCRYPT_KEY_HANDLE& key) {
    uint8_t material[32]{};
    const bool ok = Derive(token, purpose, hostNonce, viewerNonce, material) &&
        BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0, material, sizeof material, 0) >= 0;
    SecureZeroMemory(material, sizeof material);
    return ok;
}

bool Transform(BCRYPT_KEY_HANDLE key, uint64_t& sequence, uint8_t* input, ULONG size,
               uint8_t* output, uint8_t* tag, bool encrypt) {
    if (sequence == UINT64_MAX) return false;
    uint8_t nonce[12]{};
    for (size_t i = 0; i < sizeof sequence; ++i)
        nonce[i + 4] = static_cast<uint8_t>(sequence >> (i * 8));
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth{};
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = nonce;
    auth.cbNonce = sizeof nonce;
    auth.pbTag = tag;
    auth.cbTag = kTagSize;
    ULONG written = 0;
    const auto result = encrypt
        ? BCryptEncrypt(key, input, size, &auth, nullptr, 0, output, size, &written, 0)
        : BCryptDecrypt(key, input, size, &auth, nullptr, 0, output, size, &written, 0);
    if (result < 0 || written != size) return false;
    ++sequence;
    return true;
}
}

bool ConsentViewerProof(const std::string& token, const uint8_t* hostNonce,
                        const uint8_t* viewerNonce, uint8_t out[32]) {
    return Derive(token, "windowsstream-consent-viewer-proof-v3", hostNonce, viewerNonce, out);
}

ConsentCipher::~ConsentCipher() {
    if (read_) BCryptDestroyKey(read_);
    if (write_) BCryptDestroyKey(write_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
}

bool ConsentCipher::Initialize(const std::string& token, const uint8_t* hostNonce, const uint8_t* viewerNonce, bool host) {
    constexpr auto viewerWrite = "windowsstream-consent-viewer-write-v3";
    constexpr auto hostWrite = "windowsstream-consent-host-write-v3";
    return BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptSetProperty(algorithm_, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0 &&
        MakeKey(algorithm_, token, host ? hostWrite : viewerWrite, hostNonce, viewerNonce, write_) &&
        MakeKey(algorithm_, token, host ? viewerWrite : hostWrite, hostNonce, viewerNonce, read_);
}

bool ConsentCipher::Seal(uint8_t type, const void* data, size_t size, std::vector<uint8_t>& sealed) {
    if (!Active() || size > kMaxNativeSize - kConsentRecordOverhead) return false;
    std::vector<uint8_t> plain(size + 1);
    plain[0] = type;
    if (size) std::memcpy(plain.data() + 1, data, size);
    sealed.resize(size + kConsentRecordOverhead);
    const bool ok = Transform(write_, written_, plain.data(), static_cast<ULONG>(plain.size()),
        sealed.data(), sealed.data() + plain.size(), true);
    if (type == MsgKey) SecureZeroMemory(plain.data(), plain.size());
    return ok;
}

bool ConsentCipher::Open(uint8_t& type, std::vector<uint8_t>& sealed) {
    if (!Active() || sealed.size() < kConsentRecordOverhead || sealed.size() > kMaxNativeSize) return false;
    const auto size = static_cast<ULONG>(sealed.size() - kTagSize);
    std::vector<uint8_t> plain(size);
    if (!Transform(read_, received_, sealed.data(), size, plain.data(), sealed.data() + size, false)) {
        SecureZeroMemory(plain.data(), plain.size());
        return false;
    }
    type = plain[0];
    std::memmove(plain.data(), plain.data() + 1, plain.size() - 1);
    plain.back() = 0;
    plain.pop_back();
    sealed.swap(plain);
    return true;
}
