#pragma once

#include "../net.h"
#include "../../shared/file_drag.h"
#include <condition_variable>
#include <memory>
#include <optional>
#include <thread>

namespace filedrag {
constexpr UINT kUploadChanged = WM_APP + 10;
struct Source;

class Upload {
public:
    void Register(HWND viewer);
    void Revoke();
    void Stop();
    void Connect(const std::shared_ptr<Connection>& connection);
    void Disconnect();
    bool Receive(uint8_t type, const std::vector<uint8_t>& payload);
    const wchar_t* Status();

private:
    class Target;
    struct Work {
        uint32_t id;
        std::shared_ptr<Connection> connection;
        std::vector<std::wstring> paths;
    };
    struct Reply {
        UploadStatus status;
        uint16_t completed;
        DWORD error;
    };

    bool CanDrop();
    bool Queue(std::vector<std::wstring> paths);
    void SetStatus(const wchar_t* status, ULONGLONG duration = 0);
    void Run();
    bool Transfer(const Work& work);
    bool SendBlocks(const Work& work, const std::vector<Source>& files);
    bool Active(const Work& work);
    bool Wait(const Work& work, bool forAcceptance);
    static bool Send(const Work& work, uint8_t type, const Writer& payload);

    HWND viewer_ = nullptr;
    bool registered_ = false;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<Connection> connection_;
    std::optional<Work> pending_;
    std::optional<Reply> reply_;
    uint32_t next_ = 0;
    uint32_t active_ = 0;
    bool running_ = false;
    bool available_ = false;
    bool accepted_ = false;
    const wchar_t* status_ = L"";
    ULONGLONG statusUntil_ = 0;
};
}
