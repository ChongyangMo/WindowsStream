#include "locate.h"
#include "crypto.h"
#include <bcrypt.h>

namespace locate {
namespace {
constexpr char kRequest[] = "WSFIND2";
constexpr char kAnswer[] = "WSCTRL2";

bool Proof(const std::string& token, const char* context, const uint8_t* nonce, const uint8_t* port, uint8_t out[32]) {
    Writer input;
    input.append(context, strlen(context));
    input.append(nonce, 16);
    if (port) input.append(port, 2);
    return HmacSha256(token, input.bytes.data(), input.bytes.size(), out);
}
}

std::vector<uint8_t> Request(const std::string& token, uint8_t nonce[16]) {
    uint8_t proof[32]{};
    if (BCryptGenRandom(nullptr, nonce, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !Proof(token, "windowsstream-find-controller", nonce, nullptr, proof)) return {};
    Writer request;
    request.append(kRequest, 7);
    request.append(nonce, 16);
    request.append(proof, sizeof proof);
    return request.bytes;
}

std::vector<uint8_t> Answer(const std::string& token, const uint8_t* data, size_t size, uint16_t port) {
    uint8_t expected[32]{};
    if (size != 7 + 16 + 32 || memcmp(data, kRequest, 7) != 0 ||
        !Proof(token, "windowsstream-find-controller", data + 7, nullptr, expected) ||
        !SecureEqual(expected, data + 23, 32)) return {};
    Writer answer;
    answer.append(kAnswer, 7);
    answer.u16(port);
    uint8_t proof[32]{};
    if (!Proof(token, "windowsstream-controller-here", data + 7, answer.bytes.data() + 7, proof)) return {};
    answer.append(proof, sizeof proof);
    return answer.bytes;
}

bool CheckAnswer(const std::string& token, const uint8_t nonce[16], const uint8_t* data, size_t size, uint16_t& port) {
    uint8_t expected[32]{};
    if (size != 7 + 2 + 32 || memcmp(data, kAnswer, 7) != 0 ||
        !Proof(token, "windowsstream-controller-here", nonce, data + 7, expected) ||
        !SecureEqual(expected, data + 9, 32)) return false;
    port = ReadU16(data + 7);
    return port != 0;
}
}
