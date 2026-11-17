#include "input_diagnostics.h"
#include "input.h"
#include <cstdio>

namespace {
std::string Integrity(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return "未知";
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
    DWORD size = 0;
    const bool read = GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &size) != FALSE;
    CloseHandle(token);
    if (!read) return "未知";
    const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
    const auto count = *GetSidSubAuthorityCount(sid);
    return count ? std::to_string(*GetSidSubAuthority(sid, count - 1)) : "未知";
}

std::string DescribeWindow(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return "无";
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::to_string(pid) + "/完整性未知";
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string name = std::to_string(pid);
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const auto* separator = wcsrchr(path, L'\\');
        name = WideToUtf8(separator ? separator + 1 : path);
    }
    const auto integrity = Integrity(process);
    CloseHandle(process);
    return name + "/完整性" + integrity;
}
}

std::string InputDiagnostics::MouseButton(const std::vector<uint8_t>& payload, bool submitted, bool exclusive) {
    if (payload.size() != 6 || payload[0] >= 5 || payload[1] > 1 || (!payload[1] && submitted)) return {};
    const auto now = GetTickCount64();
    const auto foreground = GetForegroundWindow();
    const auto point = PrimaryInputPoint(ReadU16(payload.data() + 2), ReadU16(payload.data() + 4));
    const auto target = WindowFromPoint(point);
    if (now < nextReport_ && previousForeground_ == foreground && previousTarget_ == target) return {};
    nextReport_ = now + 500;
    previousForeground_ = foreground;
    previousTarget_ = target;

    GUITHREADINFO info{sizeof info};
    const auto thread = GetWindowThreadProcessId(foreground, nullptr);
    const bool haveGuiInfo = thread && GetGUIThreadInfo(thread, &info);
    const auto windowMode = info.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE);
    const auto active = DescribeWindow(foreground);
    const auto hit = target == foreground ? active : DescribeWindow(target);
    static const auto hostIntegrity = Integrity(GetCurrentProcess());
    char message[1400]{};
    snprintf(message, sizeof message,
        "鼠标回执：按钮 %u，%s，提交 %s；被控端完整性 %s，前台 %s，命中 %s；前台菜单或移动状态 %s，鼠标捕获 %s，输入桌面 %s，独占 %s",
        static_cast<unsigned>(payload[0]), payload[1] ? "按下" : "松开", submitted ? "成功" : "失败",
        hostIntegrity.c_str(), active.c_str(), hit.c_str(), haveGuiInfo ? std::to_string(windowMode).c_str() : "未知",
        haveGuiInfo ? (info.hwndCapture ? "有" : "无") : "未知",
        servermode::DesktopAccessible() ? "可用" : "不可用", exclusive ? "开启" : "关闭");
    return message;
}
