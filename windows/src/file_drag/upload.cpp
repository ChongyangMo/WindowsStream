#include "upload.h"
#include "../../shared/file_drag_source.h"
#include <chrono>
#include <wrl/implements.h>

namespace filedrag {
class Upload::Target final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
public:
    explicit Target(Upload& upload) : upload_(upload) {}

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        auto format = DropPathFormat();
        hasPaths_ = data->QueryGetData(&format) == S_OK;
        return DragOver(0, point, effect);
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = hasPaths_ && (*effect & DROPEFFECT_COPY) && upload_.CanDrop() ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override { hasPaths_ = false; return S_OK; }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        const bool copy = (*effect & DROPEFFECT_COPY) && upload_.CanDrop();
        *effect = copy && upload_.Queue(PathsFromDrop(data)) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        hasPaths_ = false;
        return S_OK;
    }

private:
    Upload& upload_;
    bool hasPaths_ = false;
};

void Upload::Register(HWND viewer) {
    auto target = Microsoft::WRL::Make<Target>(*this);
    const HRESULT result = target ? RegisterDragDrop(viewer, target.Get()) : E_OUTOFMEMORY;
    if (FAILED(result)) {
        LogLine("无法注册文件拖入，错误码 0x%08X", static_cast<unsigned>(result));
        return;
    }
    viewer_ = viewer;
    registered_ = running_ = true;
    worker_ = std::thread(&Upload::Run, this);
}

void Upload::Revoke() {
    if (registered_) RevokeDragDrop(viewer_);
    registered_ = false;
}

void Upload::Stop() {
    Revoke();
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        changed_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void Upload::Connect(const std::shared_ptr<Connection>& connection) {
    std::lock_guard lock(mutex_);
    connection_ = connection;
}

void Upload::Disconnect() {
    std::lock_guard lock(mutex_);
    available_ = false;
    connection_.reset();
    changed_.notify_all();
}

bool Upload::Receive(uint8_t type, const std::vector<uint8_t>& payload) {
    if (type != MsgFileUploadAvailable && type != MsgFileUploadResult) return false;
    std::lock_guard lock(mutex_);
    if (type == MsgFileUploadAvailable) {
        if (payload.empty()) available_ = true;
    } else if (payload.size() == 11 && active_ && ReadU32(payload.data()) == active_ &&
               payload[4] <= static_cast<uint8_t>(UploadStatus::Failed) && !reply_) {
        const auto status = static_cast<UploadStatus>(payload[4]);
        if (status == UploadStatus::Accepted) accepted_ = true;
        else reply_ = Reply{status, ReadU16(payload.data() + 5), ReadU32(payload.data() + 7)};
        changed_.notify_all();
    }
    return true;
}

bool Upload::CanDrop() {
    std::lock_guard lock(mutex_);
    return running_ && connection_ && available_ && !active_;
}

bool Upload::Queue(std::vector<std::wstring> paths) {
    std::lock_guard lock(mutex_);
    if (!running_ || !connection_ || !available_ || active_) return false;
    if (paths.empty()) {
        LogLine("未读取到拖入的文件，每次最多发送 %u 个文件", kMaxFiles);
        SetStatus(L"无法读取拖入的文件", 5000);
        return false;
    }
    if (++next_ == 0) ++next_;
    active_ = next_;
    accepted_ = false;
    reply_.reset();
    pending_.emplace(Work{active_, connection_, std::move(paths)});
    SetStatus(L"正在发送文件…");
    changed_.notify_one();
    return true;
}

void Upload::SetStatus(const wchar_t* status, ULONGLONG duration) {
    status_ = status;
    statusUntil_ = duration ? GetTickCount64() + duration : 0;
    PostMessageW(viewer_, kUploadChanged, 0, 0);
}

const wchar_t* Upload::Status() {
    std::lock_guard lock(mutex_);
    return statusUntil_ && GetTickCount64() >= statusUntil_ ? L"" : status_;
}

void Upload::Run() {
    std::unique_lock lock(mutex_);
    while (running_) {
        changed_.wait(lock, [&] { return !running_ || pending_.has_value(); });
        if (!running_) break;
        auto work = std::move(*pending_);
        pending_.reset();
        lock.unlock();
        const bool complete = Transfer(work);
        lock.lock();
        if (complete) LogLine("已发送 %zu 个拖拽文件到被控端下载文件夹", work.paths.size());
        else if (reply_ && reply_->status == UploadStatus::Failed) {
            LogLine("发送拖拽文件失败，被控端已保存 %u 个文件，错误码 %lu", reply_->completed, reply_->error);
        } else LogLine("发送拖拽文件失败或传输中断");
        active_ = 0;
        SetStatus(complete ? L"文件已发送" : L"文件发送失败", 5000);
    }
}

bool Upload::Transfer(const Work& work) {
    if (!Active(work)) return false;
    const auto files = OpenFiles(work.paths);
    if (files.empty()) {
        LogLine("无法打开拖入的文件，目录及不可读文件无法传输");
        return false;
    }
    Writer offer;
    offer.u32(work.id);
    offer.u16(static_cast<uint16_t>(files.size()));
    for (const auto& source : files) WriteFile(offer, source.file);
    if (!Active(work) || !Send(work, MsgFileUploadOffer, offer)) return false;
    if (Wait(work, true) && SendBlocks(work, files) && Wait(work, false)) {
        std::lock_guard lock(mutex_);
        if (reply_->status == UploadStatus::Complete && reply_->completed == files.size()) return true;
    }

    Writer cancel;
    cancel.u32(work.id);
    Send(work, MsgFileUploadCancel, cancel);
    return false;
}

bool Upload::SendBlocks(const Work& work, const std::vector<Source>& files) {
    Writer block;
    block.bytes.reserve(kUploadDataHeader + kUploadBlockSize);
    for (uint16_t index = 0; index < files.size(); ++index) {
        const auto& source = files[index];
        uint64_t offset = 0;
        do {
            if (!Active(work)) return false;
            const auto length = static_cast<DWORD>(std::min<uint64_t>(source.file.size - offset, kUploadBlockSize));
            block.bytes.clear();
            block.u32(work.id);
            block.u16(index);
            block.u64(offset);
            block.bytes.resize(kUploadDataHeader + length);
            if (!source.Read(offset, block.bytes.data() + kUploadDataHeader, length)) {
                LogLine("读取本地拖拽文件失败");
                return false;
            }
            if (!Send(work, MsgFileUploadData, block)) return false;
            offset += length;
        } while (offset < source.file.size);
    }
    return true;
}

bool Upload::Active(const Work& work) {
    std::lock_guard lock(mutex_);
    return running_ && connection_ == work.connection && !reply_;
}

bool Upload::Wait(const Work& work, bool forAcceptance) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, std::chrono::seconds(15), [&] {
        return !running_ || connection_ != work.connection || reply_ || (forAcceptance && accepted_);
    });
    if (forAcceptance) return running_ && connection_ == work.connection && accepted_ && !reply_;
    return reply_.has_value();
}

bool Upload::Send(const Work& work, uint8_t type, const Writer& payload) {
    if (work.connection->Send(type, payload.bytes.data(), payload.bytes.size())) return true;
    work.connection->Shutdown();
    return false;
}
}
