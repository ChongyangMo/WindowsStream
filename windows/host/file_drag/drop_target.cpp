#include "capture.h"
#include "../../shared/file_drag_source.h"
#include <wrl/implements.h>
#include <chrono>
#include <condition_variable>
#include <string_view>

namespace filedrag {
namespace {
constexpr UINT kShow = WM_APP + 1;
constexpr UINT kHide = WM_APP + 2;
constexpr std::wstring_view kWindowClass = L"WindowsStream.FileDragTarget";

}

struct Capture::Request {
    uint32_t id;
    bool done = false;
    std::vector<std::wstring> paths;
    std::condition_variable ready;
};

class Capture::Target final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
public:
    explicit Target(Capture& owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD buttons, POINTL, DWORD* effect) override {
        const bool copy = (*effect & DROPEFFECT_COPY) != 0;
        *effect = DROPEFFECT_NONE;
        if (copy && (buttons & MK_LBUTTON)) owner_.CapturePaths(data);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
private:
    Capture& owner_;
};

Capture::~Capture() {
    if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
    if (started_) CloseHandle(started_);
}

bool Capture::Start() {
    started_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!started_) return false;
    thread_ = std::thread(&Capture::Run, this);
    WaitForSingleObject(started_, INFINITE);
    return window_ != nullptr;
}

std::vector<std::wstring> Capture::Probe(uint32_t id) {
    const auto request = std::make_shared<Request>();
    request->id = id;
    std::unique_lock lock(mutex_);
    request_ = request;
    if (PostMessageW(window_, kShow, id, 0)) {
        request->ready.wait_for(lock, std::chrono::milliseconds(500), [&] { return request->done; });
    }
    request_.reset();
    return std::move(request->paths);
}

void Capture::Hide(uint32_t id) { PostMessageW(window_, kHide, id, 0); }

void Capture::Show(HWND window, uint32_t id) {
    {
        std::lock_guard lock(mutex_);
        if (!request_ || request_->id != id) return;
    }
    POINT point{};
    if (!GetCursorPos(&point)) return;
    visible_ = id;
    SetWindowPos(window, HWND_TOPMOST, point.x - 16, point.y - 16, 33, 33, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // 已进入拖拽循环的程序需要收到新的鼠标位置，才会查询覆盖在指针下的接收窗口。
    POINT next{point.x + 1, point.y};
    if (!MonitorFromPoint(next, MONITOR_DEFAULTTONULL)) --point.x;
    else point = next;
    SetCursorPos(point.x, point.y);
}

void Capture::CapturePaths(IDataObject* data) {
    std::shared_ptr<Request> request;
    {
        std::lock_guard lock(mutex_);
        if (!request_ || request_->id != visible_ || request_->done) return;
        request = request_;
    }
    auto paths = PathsFromDrop(data);
    std::lock_guard lock(mutex_);
    request->paths = std::move(paths);
    request->done = true;
    request->ready.notify_one();
}

LRESULT CALLBACK Capture::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    const auto self = reinterpret_cast<Capture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case kShow: self->Show(window, static_cast<uint32_t>(wParam)); return 0;
        case kHide:
            if (self->visible_ == wParam) {
                self->visible_ = 0;
                ShowWindow(window, SW_HIDE);
            }
            return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_CLOSE: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void Capture::Run() {
    const bool ole = SUCCEEDED(OleInitialize(nullptr));
    HWND window = nullptr;
    if (ole) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kWindowClass.data();
        if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
        }
    }
    auto target = Microsoft::WRL::Make<Target>(*this);
    if (window && target && SetLayeredWindowAttributes(window, 0, 1, LWA_ALPHA) &&
        SUCCEEDED(RegisterDragDrop(window, target.Get()))) window_ = window;
    SetEvent(started_);
    if (window_) {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        RevokeDragDrop(window);
    }
    if (window) DestroyWindow(window);
    if (ole) OleUninitialize();
}
}
