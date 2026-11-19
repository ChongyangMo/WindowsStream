#include "session.h"
#include "../../shared/remote_files.h"
#include <array>
#include <chrono>

namespace remotefiles {
void ViewerFiles::Start(const std::wstring& profile) {
    stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto name = PipeName(profile);
    if (!stop_ || name.empty()) {
        LogLine("无法初始化后台日志读取接口");
        return;
    }
    worker_ = std::thread(&ViewerFiles::Serve, this, name);
}

void ViewerFiles::Stop() {
    if (stop_) SetEvent(stop_.get());
    Disconnect();
    if (worker_.joinable()) worker_.join();
    stop_.reset();
}

void ViewerFiles::Connect(const std::shared_ptr<Connection>& connection) {
    std::lock_guard lock(mutex_);
    connection_ = connection;
    available_ = false;
}

void ViewerFiles::Disconnect() {
    {
        std::lock_guard lock(mutex_);
        connection_.reset();
        available_ = false;
        pending_ = 0;
    }
    changed_.notify_all();
}

bool ViewerFiles::Receive(uint8_t type, std::vector<uint8_t>& payload) {
    if (type == MsgRemoteFilesAvailable) {
        std::lock_guard lock(mutex_);
        available_ = connection_ && payload.empty();
        return true;
    }
    if (type != MsgRemoteFilesResult) return false;
    if (payload.size() >= kResultHeader && payload.size() <= kResultHeader + kMaxContent) {
        std::lock_guard lock(mutex_);
        if (pending_ && ReadU64(payload.data()) == pending_ && response_.empty()) {
            response_.swap(payload);
            changed_.notify_all();
        }
    }
    return true;
}

Writer ViewerFiles::Request(const uint8_t* data, DWORD size) {
    std::unique_lock lock(mutex_);
    if (!size || size > kMaxTarget + 1) return Result(0, ERROR_INVALID_DATA);
    if (!connection_) return Result(0, ERROR_NOT_CONNECTED);
    if (!available_) return Result(0, ERROR_NOT_SUPPORTED);
    const auto connection = connection_;
    const auto id = ++next_;
    pending_ = id;
    response_.clear();
    Writer message;
    message.u64(id);
    message.append(data, size);
    lock.unlock();
    const bool sent = connection->Send(MsgRemoteFilesRequest, message.bytes.data(), message.bytes.size());
    lock.lock();
    if (sent) changed_.wait_for(lock, std::chrono::seconds(10), [&] { return !response_.empty() || pending_ != id; });
    const bool stillConnected = pending_ == id;
    pending_ = 0;
    if (!response_.empty()) {
        Writer response;
        response.bytes.swap(response_);
        return response;
    }
    return Result(id, stillConnected && sent ? ERROR_TIMEOUT : ERROR_NOT_CONNECTED);
}

void ViewerFiles::Serve(const std::wstring& name) {
    const auto pipe = CreateServerPipe(name);
    if (!pipe) {
        LogLine("无法创建后台日志读取接口");
        return;
    }
    while (WaitForSingleObject(stop_.get(), 0) == WAIT_TIMEOUT) {
        if (!AcceptClient(pipe.get(), stop_.get())) break;
        std::array<uint8_t, kMaxTarget + 1> input{};
        DWORD transferred = 0;
        if (Transfer(pipe.get(), stop_.get(), false, input.data(), static_cast<DWORD>(input.size()), transferred, 2500)) {
            auto response = Request(input.data(), transferred);
            if (Transfer(pipe.get(), stop_.get(), true, response.bytes.data(), static_cast<DWORD>(response.bytes.size()), transferred, 3000)) {
                // 控制端读到结果后发送确认，避免断开时丢弃尚未读完的数据。
                uint8_t received = 0;
                Transfer(pipe.get(), stop_.get(), false, &received, 1, transferred, 1500);
            }
        }
        DisconnectNamedPipe(pipe.get());
    }
}
}
