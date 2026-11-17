#pragma once

#include "../../src/common.h"
#include <memory>
#include <thread>

namespace filedrag {
class Capture {
public:
    ~Capture();
    bool Start();
    std::vector<std::wstring> Probe(uint32_t id);
    void Hide(uint32_t id);

private:
    struct Request;
    class Target;
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void Run();
    void Show(HWND window, uint32_t id);
    void CapturePaths(IDataObject* data);

    std::thread thread_;
    std::mutex mutex_;
    HANDLE started_ = nullptr;
    HWND window_ = nullptr;
    std::shared_ptr<Request> request_;
    uint32_t visible_ = 0; // OLE 线程使用
};
}
