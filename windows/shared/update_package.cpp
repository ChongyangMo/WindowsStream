#include "update_package.h"
#include "crypto.h"
#include <bcrypt.h>

namespace updates {
std::wstring ModulePath() {
    std::wstring value(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    value.resize(length);
    return value;
}
std::string Hex(const uint8_t* data, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (size_t i = 0; i < size; ++i) { text[i * 2] = digits[data[i] >> 4]; text[i * 2 + 1] = digits[data[i] & 15]; }
    return text;
}
bool Unhex(const std::string& text, std::vector<uint8_t>& output) {
    if (text.size() % 2) return false;
    output.clear();
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        const int a = digit(text[i]), b = digit(text[i + 1]);
        if (a < 0 || b < 0) return false;
        output.push_back(static_cast<uint8_t>(a * 16 + b));
    }
    return true;
}
bool Hash(const uint8_t* data, size_t size, uint8_t output[32]) {
    return size <= UINT32_MAX && BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        const_cast<PUCHAR>(data), static_cast<ULONG>(size), output, 32) >= 0;
}
bool Inspect(Package& package) {
    const auto& data = package.bytes;
    if (data.size() < sizeof(IMAGE_NT_HEADERS64) || data.size() > kMaxPackage) return false;
    IMAGE_DOS_HEADER dos{};
    memcpy(&dos, data.data(), sizeof dos);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
        static_cast<size_t>(dos.e_lfanew) > data.size() - sizeof(IMAGE_NT_HEADERS64)) return false;
    IMAGE_NT_HEADERS64 pe{};
    memcpy(&pe, data.data() + dos.e_lfanew, sizeof pe);
    if (pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !pe.FileHeader.TimeDateStamp ||
        pe.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || !pe.FileHeader.NumberOfSections) return false;
    size_t offset = static_cast<size_t>(dos.e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) + pe.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < pe.FileHeader.NumberOfSections; ++i) {
        if (offset > data.size() - sizeof(IMAGE_SECTION_HEADER)) return false;
        IMAGE_SECTION_HEADER section{};
        memcpy(&section, data.data() + offset, sizeof section);
        if (section.PointerToRawData > data.size() || section.SizeOfRawData > data.size() - section.PointerToRawData) return false;
        offset += sizeof section;
    }
    package.stamp = pe.FileHeader.TimeDateStamp;
    return Hash(data.data(), data.size(), package.hash.data());
}
bool LoadPackage(const std::wstring& path, Package& package) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxPackage;
    if (ok) {
        package.bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, package.bytes.data(), static_cast<DWORD>(package.bytes.size()), &read, nullptr) && read == package.bytes.size();
    }
    CloseHandle(file);
    return ok && Inspect(package);
}
std::vector<uint8_t> Manifest(const Package& package, const std::string& token) {
    Writer value;
    value.u32(1); value.u64(package.bytes.size()); value.u32(package.stamp);
    value.append(package.hash.data(), package.hash.size());
    const std::string domain = "windowsstream-package-v1";
    Writer proof;
    proof.append(domain.data(), domain.size()); proof.append(value.bytes.data(), value.bytes.size());
    uint8_t digest[32]{};
    if (!HmacSha256(token, proof.bytes.data(), proof.bytes.size(), digest)) return {};
    value.append(digest, sizeof digest);
    return value.bytes;
}
bool VerifyManifest(const std::vector<uint8_t>& data, const std::string& token,
                    uint64_t& size, uint32_t& stamp, std::array<uint8_t, 32>& hash) {
    if (data.size() != 80 || ReadU32(data.data()) != 1) return false;
    const std::string domain = "windowsstream-package-v1";
    Writer proof;
    proof.append(domain.data(), domain.size()); proof.append(data.data(), 48);
    uint8_t digest[32]{};
    if (!HmacSha256(token, proof.bytes.data(), proof.bytes.size(), digest) || !SecureEqual(digest, data.data() + 48, 32)) return false;
    size = ReadU64(data.data() + 4); stamp = ReadU32(data.data() + 12);
    memcpy(hash.data(), data.data() + 16, 32);
    return size > 0 && size <= kMaxPackage && stamp != 0;
}
std::string RequestProof(const std::string& token, const std::string& method, const std::string& path,
                         const std::string& nonce, const std::vector<uint8_t>& body) {
    uint8_t hash[32]{}, digest[32]{};
    if (!Hash(body.data(), body.size(), hash)) return {};
    const auto text = "windowsstream-update-http-v1\n" + method + "\n" + path + "\n" + nonce + "\n" + Hex(hash, 32);
    if (!HmacSha256(token, reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest)) return {};
    return Hex(digest, 32);
}
std::string Nonce() {
    uint8_t bytes[16]{};
    return BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 ? Hex(bytes, sizeof bytes) : std::string();
}
}
