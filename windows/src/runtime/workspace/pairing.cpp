#include "catalog.h"
#include "../relay.h"
#include <filesystem>

namespace workspace {
namespace {
bool IsKey(const std::wstring& value) {
    return value.size() == 64 && value.find_first_not_of(L"0123456789abcdef") == std::wstring::npos;
}

bool SaveIni(const std::filesystem::path& path, const std::string& bytes) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return false;
    wchar_t suffix[40]{};
    StringFromGUID2(guid, suffix, 40);
    auto pending = path;
    pending += suffix;
    HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size();
    CloseHandle(file);
    if (saved && MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(pending.c_str());
    return false;
}
}

bool ImportRelayPair(const std::wstring& base, const std::wstring& file, Profile& profile, std::wstring& error) {
    std::error_code status;
    const auto size = std::filesystem::file_size(file, status);
    if (status || !size || size > 16 * 1024) {
        error = L"无法读取 WindowsStream 配对文件";
        return false;
    }
    const auto name = ReadSetting(file, L"pairing", L"name");
    const auto device = ReadSetting(file, L"pairing", L"device");
    const auto viewerKey = ReadSetting(file, L"pairing", L"viewer_key");
    const auto streamKey = ReadSetting(file, L"pairing", L"stream_key");
    if (name.empty() || name.size() > 63 ||
        name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::wstring::npos ||
        !IsKey(device) || !IsKey(viewerKey) || !IsKey(streamKey)) {
        error = L"WindowsStream 配对文件无效";
        return false;
    }

    const auto folder = std::filesystem::path(base).parent_path() / L"relay-devices" / device;
    const auto client = folder / L"client.ini";
    const auto relay = folder / L"relay.ini";
    profile = {name + L"（中转）", client.wstring(), false};
    if (GetFileAttributesW(client.c_str()) != INVALID_FILE_ATTRIBUTES && !ViewerRelayConfig(profile.path).empty()) {
        if (ReadSetting(relay.wstring(), L"relay", L"token") != viewerKey ||
            ReadSetting(profile.path, L"connection", L"token") != streamKey) {
            error = L"这台设备已有不同的配对配置";
            return false;
        }
        profile.name = ReadSetting(profile.path, L"connection", L"name", profile.name.c_str());
        return true;
    }

    const auto network = std::filesystem::path(ExecutablePath()).parent_path() / L".runtime" / L"relay-network.ini";
    const auto proxy = ReadSetting(network.wstring(), L"relay", L"http_proxy");
    const auto relayIni = L"[relay]\r\nrole=viewer\r\ndevice=" + device +
        L"\r\nendpoint=wss://sdserver.luxvisions-inno.com/api/windowsstream/relay"
        L"\r\nserver_address=10.18.32.67:443\r\nhttp_proxy=" + proxy +
        L"\r\nlocal_address=127.0.0.1:7400\r\ntoken=" + viewerKey + L"\r\n";
    const auto clientIni = L"\uFEFF[connection]\r\nname=" + profile.name +
        L"\r\ninstance=relay-" + device.substr(0, 32) +
        L"\r\nhost=127.0.0.1\r\nport=7400\r\ntoken=" + streamKey +
        L"\r\n\r\n[proxy]\r\nenabled=0\r\n\r\n[updates]\r\nenabled=0\r\n";
    std::filesystem::create_directories(folder, status);
    const std::string clientBytes(reinterpret_cast<const char*>(clientIni.data()), clientIni.size() * sizeof(wchar_t));
    if (status || !SaveIni(relay, WideToUtf8(relayIni)) || !SaveIni(client, clientBytes)) {
        error = L"无法保存中转连接";
        return false;
    }
    LogLine("已导入中转配对：%s", WideToUtf8(profile.name).c_str());
    return true;
}
}
