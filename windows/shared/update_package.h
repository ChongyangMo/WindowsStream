#pragma once
#include "../src/common.h"
#include <array>

namespace updates {
constexpr size_t kMaxPackage = 64 * 1024 * 1024;
struct Package {
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 32> hash{};
    uint32_t stamp = 0;
};
std::wstring ModulePath();
std::string Hex(const uint8_t* data, size_t size);
bool Unhex(const std::string& text, std::vector<uint8_t>& output);
bool Hash(const uint8_t* data, size_t size, uint8_t output[32]);
bool Inspect(Package& package);
bool LoadPackage(const std::wstring& path, Package& package);
std::vector<uint8_t> Manifest(const Package& package, const std::string& token);
bool VerifyManifest(const std::vector<uint8_t>& data, const std::string& token, uint64_t& size, uint32_t& stamp, std::array<uint8_t, 32>& hash);
std::string RequestProof(const std::string& token, const std::string& method, const std::string& path,
                         const std::string& nonce, const std::vector<uint8_t>& body);
std::string Nonce();
}
