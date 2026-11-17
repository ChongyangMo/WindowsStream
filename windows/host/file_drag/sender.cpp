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
        case MsgFileDragAccept:
            if (enabled_ && payload.size() == 4) {
                const auto offer = offers_.find(ReadU32(payload.data()));
                if (offer != offers_.end() && !offer->second.accepted) {
                    if (input.CanHandOffDrag() && input.FinishFileDrag()) offer->second.accepted = true;
                    else offers_.erase(offer);
                }
            }
            return true;
        default:
            return false;
    }
}

void Sender::Probe(uint32_t id, InputInjector& input) {
    std::vector<Source> files;
    if (input.CanHandOffDrag() && offers_.size() < 8 && !offers_.contains(id) && servermode::DesktopAccessible()) {
        const auto paths = capture_.Probe(id);
        capture_.Hide(id);
        if (!paths.empty()) {
            files = OpenFiles(paths);
            if (files.empty()) {
                LogLine("无法读取本次拖拽的文件，目录及不可读文件无法传输");
            }
        }
    }
    Writer offer;
    offer.u32(id);
    offer.u16(static_cast<uint16_t>(files.size()));
    for (const auto& source : files) WriteFile(offer, source.file);
    if (!files.empty()) offers_.emplace(id, OfferedFiles{std::move(files)});
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
    if (offer != offers_.end() && offer->second.accepted && index < offer->second.files.size() && length && length <= kReadSize) {
        const auto& source = offer->second.files[index];
        if (offset < source.file.size && length <= source.file.size - offset) {
            response.bytes.resize(5 + length);
            if (source.Read(offset, response.bytes.data() + 5, length)) {
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
