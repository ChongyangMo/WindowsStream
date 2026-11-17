#pragma once

#include "../src/common.h"

namespace filedrag {
constexpr uint16_t kMaxFiles = 256;
constexpr uint32_t kReadSize = 512 * 1024;
constexpr uint32_t kUploadBlockSize = 128 * 1024;
constexpr size_t kUploadDataHeader = 14;  // u32 批次、u16 文件序号、u64 偏移
enum class UploadStatus : uint8_t { Accepted, Complete, Failed };

struct File {
    std::wstring name;
    uint64_t size = 0;
    uint64_t modified = 0;
};

inline bool ValidName(const std::wstring& name) {
    if (name.empty() || name.size() >= MAX_PATH || name.back() == L'.' || name.back() == L' ') return false;
    if (std::any_of(name.begin(), name.end(), [](wchar_t ch) {
        return ch < 32 || wcschr(L"<>:\"/\\|?*", ch) != nullptr;
    })) return false;
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
        if (_wcsicmp(stem.c_str(), device) == 0) return false;
    }
    return stem.size() != 4 || !wcschr(L"123456789¹²³", stem.back()) ||
        (_wcsnicmp(stem.c_str(), L"COM", 3) != 0 && _wcsnicmp(stem.c_str(), L"LPT", 3) != 0);
}

inline void WriteFile(Writer& offer, const File& file) {
    offer.u64(file.size);
    offer.u64(file.modified);
    offer.u16(static_cast<uint16_t>(file.name.size()));
    for (wchar_t ch : file.name) offer.u16(ch);
}

inline std::vector<File> ReadOffer(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 6) return {};
    const auto count = ReadU16(bytes.data() + 4);
    if (count > kMaxFiles) return {};
    size_t pos = 6;
    std::vector<File> files;
    files.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (bytes.size() - pos < 18) return {};
        File file;
        file.size = ReadU64(bytes.data() + pos);
        file.modified = ReadU64(bytes.data() + pos + 8);
        const auto length = ReadU16(bytes.data() + pos + 16);
        pos += 18;
        if (length >= MAX_PATH || bytes.size() - pos < length * 2u) return {};
        file.name.reserve(length);
        for (size_t j = 0; j < length; ++j, pos += 2) file.name.push_back(ReadU16(bytes.data() + pos));
        if (!ValidName(file.name) || file.size > INT64_MAX) return {};
        files.push_back(std::move(file));
    }
    if (pos != bytes.size()) return {};
    return files;
}
}
