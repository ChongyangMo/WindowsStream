#include "transport.h"
#include "../crypto.h"
#include <bcrypt.h>

namespace proxy {
bool Authenticate(SOCKET socket, const std::string& token, bool controller) {
    if (token.size() < 32) return false;
    Timeout(socket, 5000);
    uint8_t hello[40]{'W','S','P','X','Y','1','\r','\n'}, answer[64]{}, proof[32]{};
    auto mac = [&](const char* role, uint8_t out[32]) {
        Writer material; material.append(role, strlen(role));
        material.append(hello, sizeof hello); material.append(answer, 32);
        return HmacSha256(token, material.bytes.data(), material.bytes.size(), out);
    };
    if (controller) {
        if (BCryptGenRandom(nullptr, hello + 8, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
            !Send(socket, hello, sizeof hello) || !Read(socket, answer, sizeof answer) ||
            !mac("windowsstream-proxy-host-v1", proof) || !SecureEqual(proof, answer + 32, 32) ||
            !mac("windowsstream-proxy-controller-v1", proof) || !Send(socket, proof, sizeof proof)) return false;
        uint8_t ack = 0; return Read(socket, &ack, 1) && ack == 1;
    }
    if (!Read(socket, hello, sizeof hello) || memcmp(hello, "WSPXY1\r\n", 8) ||
        BCryptGenRandom(nullptr, answer, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !mac("windowsstream-proxy-host-v1", answer + 32) || !Send(socket, answer, sizeof answer) ||
        !Read(socket, proof, sizeof proof)) return false;
    uint8_t expected[32]{};
    if (!mac("windowsstream-proxy-controller-v1", expected) || !SecureEqual(proof, expected, 32)) return false;
    const uint8_t ack = 1; return Send(socket, &ack, 1);
}
}
