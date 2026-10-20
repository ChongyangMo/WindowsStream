#pragma once
#include "../src/common.h"

// 反向模式自动发现：被控端广播寻找控制端，请求与应答都用配对口令签名，见 PROTOCOL.md。
namespace locate {
constexpr uint16_t kPort = 7401;
std::vector<uint8_t> Request(const std::string& token, uint8_t nonce[16]);
// 校验通过时返回应答报文，否则为空。
std::vector<uint8_t> Answer(const std::string& token, const uint8_t* data, size_t size, uint16_t port);
bool CheckAnswer(const std::string& token, const uint8_t nonce[16], const uint8_t* data, size_t size, uint16_t& port);
}
