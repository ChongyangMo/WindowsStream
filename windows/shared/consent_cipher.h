#pragma once

#include "../src/common.h"
#include <bcrypt.h>

constexpr uint32_t kConsentProtocolVersion = 3;
constexpr auto kConsentHostProof = "windowsstream-consent-host-v3";
constexpr size_t kConsentNonceSize = 32;
constexpr size_t kConsentRecordOverhead = 17;

bool ConsentViewerProof(const std::string& token, const uint8_t* hostNonce,
                        const uint8_t* viewerNonce, uint8_t out[32]);

class ConsentCipher {
public:
    ConsentCipher() = default;
    ~ConsentCipher();
    ConsentCipher(const ConsentCipher&) = delete;
    ConsentCipher& operator=(const ConsentCipher&) = delete;

    bool Initialize(const std::string& token, const uint8_t* hostNonce, const uint8_t* viewerNonce, bool host);
    bool Active() const { return write_ && read_; }
    bool Seal(uint8_t type, const void* data, size_t size, std::vector<uint8_t>& sealed);
    bool Open(uint8_t& type, std::vector<uint8_t>& sealed);

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_KEY_HANDLE write_ = nullptr, read_ = nullptr;
    uint64_t written_ = 0, received_ = 0;
};
