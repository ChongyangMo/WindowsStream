#include "sender.h"
#include "../channel.h"
#include "../input.h"
#include "../service/service.h"

namespace filedrag {
bool Sender::Start() {
    enabled_ = capture_.Start();
    if (!enabled_) {
        LogLine("无法初始化文件拖拽接收窗口");
        return true;
    }
    return channel_.Send(MsgFileDragAvailable, Writer{});
}

bool Sender::Handle(uint8_t type, const std::vector<uint8_t>& payload, InputInjector& input) {
    switch (type) {
        case MsgFileDragProbe:
            if (enabled_ && payload.size() == 4 && ReadU32(payload.data())) Probe(ReadU32(payload.data()), input);
            return true;
        case MsgFileDragRead:
            if (enabled_ && payload.size() == 22) Read(payload);
            return true;
        case MsgFileDragClose:
            if (enabled_ && payload.size() == 4) offers_.erase(ReadU32(payload.data()));
            return true;
        default:
            return false;
    }
}

std::vector<Sender::Source> Sender::OpenFiles(const std::vector<std::wstring>& paths) {
    std::vector<Source> files;
    files.reserve(paths.size());
    for (const auto& path : paths) {
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        auto name = path.substr(slash + 1);
        if (!ValidName(name)) return {};
        const auto raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return {};
        Source source;
        source.handle.reset(raw);
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(raw) != FILE_TYPE_DISK || !GetFileInformationByHandle(raw, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
        source.file = {std::move(name),
            (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
            (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime};
        files.push_back(std::move(source));
    }
    return files;
}

void Sender::Probe(uint32_t id, InputInjector& input) {
    std::vector<Source> files;
    if (input.CanHandOffDrag() && offers_.size() < 8 && !offers_.contains(id) && servermode::DesktopAccessible()) {
        const auto paths = capture_.Probe(id);
        if (!paths.empty()) {
            files = OpenFiles(paths);
            if (files.empty()) {
                LogLine("无法读取本次拖拽的文件，目录及不可读文件无法传输");
            } else if (!input.FinishFileDrag()) {
                files.clear();
            }
        }
        capture_.Hide(id);
    }
    Writer offer;
    offer.u32(id);
    offer.u16(static_cast<uint16_t>(files.size()));
    for (const auto& source : files) WriteFile(offer, source.file);
    if (!files.empty()) offers_.emplace(id, std::move(files));
    if (!channel_.Send(MsgFileDragOffer, offer)) channel_.Shutdown();
}

void Sender::Read(const std::vector<uint8_t>& payload) {
    const auto id = ReadU32(payload.data());
    const auto request = ReadU32(payload.data() + 4);
    const auto index = ReadU16(payload.data() + 8);
    const auto offset = ReadU64(payload.data() + 10);
    const auto length = ReadU32(payload.data() + 18);
    const auto offer = offers_.find(id);

    Writer response;
    response.u32(request);
    response.u8(0);
    if (offer != offers_.end() && index < offer->second.size() && length && length <= kReadSize) {
        const auto& source = offer->second[index];
        if (offset < source.file.size && length <= source.file.size - offset) {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            DWORD received = 0;
            response.bytes.resize(5 + length);
            const auto handle = source.handle.get();
            if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) &&
                ReadFile(handle, response.bytes.data() + 5, length, &received, nullptr) && received == length) {
                response.bytes[4] = 1;
            } else {
                response.bytes.resize(5);
                LogLine("读取被控端拖拽文件失败");
            }
        }
    }
    if (!channel_.Send(MsgFileDragData, response)) channel_.Shutdown();
}
}
