#include "input.h"
#include "exclusive_mouse.h"

InputInjector::~InputInjector() { ReleaseAll(); }

bool InputInjector::FinishFileDrag() {
    // 取消原拖拽后再松开左键，避免被控机上的文件被放进任何目录。
    const bool escaped = Key(1, true) && Key(1, false);
    return Button(0, false) && escaped;
}

bool InputInjector::Submit(INPUT* inputs, UINT count) {
    for (UINT i = 0; i < count; ++i) {
        if (inputs[i].type == INPUT_MOUSE) inputs[i].mi.dwExtraInfo = kWindowsStreamMouseInput;
    }
    SetLastError(ERROR_SUCCESS);
    if ((!servermode::SecureAgent() || desktop_.Attach()) && SendInput(count, inputs, sizeof(INPUT)) == count)
        return true;
    const DWORD error = GetLastError();
    const auto now = NowMicros();
    if (!lastFailure_ || now - lastFailure_ >= 2000000) {
        failureNotice_ = error;
        lastFailure_ = now;
    }
    return false;
}

POINT PrimaryInputPoint(uint16_t x, uint16_t y) {
    return {static_cast<LONG>(static_cast<int64_t>(x) * (GetSystemMetrics(SM_CXSCREEN) - 1) / 65535),
            static_cast<LONG>(static_cast<int64_t>(y) * (GetSystemMetrics(SM_CYSCREEN) - 1) / 65535)};
}

namespace {
bool MousePosition(uint16_t x, uint16_t y, INPUT& input) {
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth < 2 || virtualHeight < 2) return false;
    const auto point = PrimaryInputPoint(x, y);
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(static_cast<int64_t>(point.x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / (virtualWidth - 1));
    input.mi.dy = static_cast<LONG>(static_cast<int64_t>(point.y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / (virtualHeight - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return true;
}
}

bool InputInjector::Move(uint16_t x, uint16_t y) {
    INPUT position{};
    return MousePosition(x, y, position) && Submit(position);
}

bool InputInjector::Key(uint16_t code, bool down) {
    // 低 8 位为扫描码，bit8 是 E0；Pause 使用 bit9 标记的 Windows 虚拟键。
    if ((code & ~0x3ffu) || !(code & 0xffu)) return false;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    if (code & 0x200) input.ki.wVk = code & 0xff;
    else {
        input.ki.wScan = code & 0xff;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | ((code & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (!Submit(input)) return false;
    if (down) keys_.insert(code);
    else keys_.erase(code);
    return true;
}

bool InputInjector::Button(uint8_t button, bool down, const INPUT* position) {
    static constexpr DWORD flags[5][2] = {
        {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN}, {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
        {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}, {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
        {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
    };
    if (button >= 5) return false;
    INPUT events[2]{};
    auto& input = events[1];
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags[button][down ? 1 : 0];
    if (button >= 3) input.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
    if (position) {
        // 在同一批提交定位和按键，避免被控机本地移动插在两次操作之间。
        events[0] = *position;
        if (!Submit(events, 2) && (down || !Submit(input))) return false;
    } else if (!Submit(input)) {
        return false;
    }
    if (down) buttons_.insert(button);
    else buttons_.erase(button);
    return true;
}

bool InputInjector::Handle(uint8_t type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MsgMouseMove:
            return payload.size() == 4 && Move(ReadU16(payload.data()), ReadU16(payload.data() + 2));
        case MsgMouseButton: {
            if (payload.size() != 6 || payload[0] >= 5 || payload[1] >= 2) return false;
            const bool down = payload[1] != 0;
            INPUT position{};
            if (MousePosition(ReadU16(payload.data() + 2), ReadU16(payload.data() + 4), position))
                return Button(payload[0], down, &position);
            // 屏幕状态变化导致定位不可用时，仍尝试松开已按下的鼠标键。
            return !down && buttons_.count(payload[0]) && Button(payload[0], false);
        }
        case MsgKey:
            return payload.size() == 3 && payload[2] < 2 && Key(ReadU16(payload.data()), payload[2] != 0);
        case MsgReleaseAll:
            if (!payload.empty()) return false;
            ReleaseAll();
            return true;
        case MsgScroll:
            if (payload.size() != 4) return false;
            for (int axis = 0; axis < 2; ++axis) {
                const auto delta = static_cast<int16_t>(ReadU16(payload.data() + axis * 2));
                if (delta) {
                    INPUT input{};
                    input.type = INPUT_MOUSE;
                    input.mi.dwFlags = axis == 0 ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
                    input.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(delta));
                    if (!Submit(input)) return false;
                }
            }
            return true;
        default:
            return false;
    }
}

void InputInjector::ReleaseAll() {
    // 使用快照，因为释放操作会从集合中删除成功释放的键。
    const auto keys = keys_;
    const auto buttons = buttons_;
    for (const auto code : keys) Key(code, false);
    for (const auto button : buttons) Button(button, false);
}
