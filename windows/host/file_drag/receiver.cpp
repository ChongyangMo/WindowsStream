#include "receiver.h"
#include "../channel.h"
#include "../service/service.h"
#include "../../shared/file_drag_source.h"
#include <shlobj.h>
#include <cstring>

namespace filedrag {
namespace {
std::wstring NumberedName(const std::wstring& name, unsigned number) {
    if (!number) return name;
    auto dot = name.find_last_of(L'.');
    if (dot == 0 || dot == std::wstring::npos) dot = name.size();
    const auto suffix = L" (" + std::to_wstring(number) + L")";
    const auto extension = name.substr(dot);
    if (extension.size() + suffix.size() >= 255) dot = name.size();
    const auto ending = suffix + name.substr(dot);
    auto stem = name.substr(0, std::min(dot, 255 - ending.size()));
    if (!stem.empty() && stem.back() >= 0xd800 && stem.back() <= 0xdbff) stem.pop_back();
    return stem + ending;
}

DWORD Rename(HANDLE handle, const std::wstring& directory, const std::wstring& name) {
    for (unsigned number = 0; number < 10000; ++number) {
        const auto path = directory + NumberedName(name, number);
        const auto bytes = static_cast<DWORD>(sizeof(FILE_RENAME_INFO) + path.size() * sizeof(wchar_t));
        auto storage = std::make_unique<uint8_t[]>(bytes);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
        info->FileNameLength = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, path.c_str(), info->FileNameLength + sizeof(wchar_t));
        if (SetFileInformationByHandle(handle, FileRenameInfo, info, bytes)) return ERROR_SUCCESS;
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return error;
    }
    return ERROR_FILE_EXISTS;
}
}

struct Receiver::Batch {
    ~Batch() {
        if (!part) return;
        FILE_DISPOSITION_INFO discard{TRUE};
        if (!SetFileInformationByHandle(part.get(), FileDispositionInfo, &discard, sizeof discard)) {
            LogLine("清理未传完的文件失败，错误码 %lu", GetLastError());
        }
    }

    DWORD OpenPart() {
        GUID guid{};
        const auto result = CoCreateGuid(&guid);
        if (FAILED(result)) return static_cast<DWORD>(result);
        wchar_t name[40]{};
        StringFromGUID2(guid, name, ARRAYSIZE(name));
        const auto temp = directory + L".WindowsStream-" + name + L".part";
        const HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return GetLastError();
        part.reset(handle);
        return ERROR_SUCCESS;
    }

    DWORD FinishFile() {
        const auto& file = files[index];
        const FILETIME modified{static_cast<DWORD>(file.modified), static_cast<DWORD>(file.modified >> 32)};
        if (file.modified && !SetFileTime(part.get(), nullptr, nullptr, &modified)) return GetLastError();
        const auto error = Rename(part.get(), directory, file.name);
        if (error) return error;
        part.reset();
        ++index;
        offset = 0;
        return ERROR_SUCCESS;
    }

    uint32_t id = 0;
    std::wstring directory;
    std::vector<File> files;
    uint16_t index = 0;
    uint64_t offset = 0;
    FileHandle part;
};

Receiver::Receiver(HostChannel& channel) : channel_(channel) {}
Receiver::~Receiver() = default;

bool Receiver::Start() {
    enabled_ = true;
    return channel_.Send(MsgFileUploadAvailable, Writer{});
}

bool Receiver::Handle(uint8_t type, const std::vector<uint8_t>& payload) {
    if (type != MsgFileUploadOffer && type != MsgFileUploadData && type != MsgFileUploadCancel) return false;
    if (!enabled_ || payload.size() < 4) return true;
    const auto id = ReadU32(payload.data());
    if (!id) return true;
    if (type == MsgFileUploadOffer) Offer(payload, id);
    else if (batch_ && batch_->id == id) {
        if (type == MsgFileUploadData) Write(payload);
        else if (payload.size() == 4) Finish(ERROR_CANCELLED);
    }
    return true;
}

void Receiver::Offer(const std::vector<uint8_t>& payload, uint32_t id) {
    if (batch_) {
        if (batch_->id == id) Finish(ERROR_INVALID_DATA);
        else Reply(id, UploadStatus::Failed, 0, ERROR_BUSY);
        return;
    }
    if (!servermode::DesktopAccessible()) {
        Reply(id, UploadStatus::Failed, 0, ERROR_ACCESS_DENIED);
        return;
    }
    auto files = ReadOffer(payload);
    if (files.empty()) {
        Reply(id, UploadStatus::Failed, 0, ERROR_INVALID_DATA);
        return;
    }
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &path);
    if (SUCCEEDED(result)) {
        batch_ = std::make_unique<Batch>();
        batch_->id = id;
        batch_->directory = ExtendedPath(path);
        if (!batch_->directory.ends_with(L'\\')) batch_->directory += L'\\';
        batch_->files = std::move(files);
    }
    CoTaskMemFree(path);
    if (!batch_) {
        Reply(id, UploadStatus::Failed, 0, static_cast<DWORD>(result));
        return;
    }
    if (const auto error = batch_->OpenPart()) Finish(error);
    else Reply(id, UploadStatus::Accepted);
}

void Receiver::Write(const std::vector<uint8_t>& payload) {
    auto& batch = *batch_;
    const auto remaining = batch.files[batch.index].size - batch.offset;
    const auto length = static_cast<DWORD>(std::min<uint64_t>(remaining, kUploadBlockSize));
    if (payload.size() != kUploadDataHeader + length || ReadU16(payload.data() + 4) != batch.index ||
        ReadU64(payload.data() + 6) != batch.offset) {
        Finish(ERROR_INVALID_DATA);
        return;
    }
    if (!batch.part) {
        if (const auto error = batch.OpenPart()) { Finish(error); return; }
    }
    DWORD written = 0;
    if (length && !::WriteFile(batch.part.get(), payload.data() + kUploadDataHeader, length, &written, nullptr)) {
        Finish(GetLastError());
        return;
    }
    if (written != length) { Finish(ERROR_WRITE_FAULT); return; }
    batch.offset += length;
    if (length == remaining) {
        if (const auto error = batch.FinishFile()) Finish(error);
        else if (batch.index == batch.files.size()) Finish(ERROR_SUCCESS);
    }
}

void Receiver::Finish(DWORD error) {
    const auto id = batch_->id;
    const auto completed = batch_->index;
    batch_.reset();
    if (error) LogLine("接收拖拽文件中断，已保存 %u 个文件，错误码 %lu", completed, error);
    else LogLine("已接收 %u 个拖拽文件到下载文件夹", completed);
    Reply(id, error ? UploadStatus::Failed : UploadStatus::Complete, completed, error);
}

void Receiver::Reply(uint32_t id, UploadStatus status, uint16_t completed, DWORD error) {
    Writer reply;
    reply.u32(id);
    reply.u8(static_cast<uint8_t>(status));
    reply.u16(completed);
    reply.u32(error);
    if (!channel_.Send(MsgFileUploadResult, reply)) channel_.Shutdown();
}
}
