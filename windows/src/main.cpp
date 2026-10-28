#include "common.h"
#include "discovery.h"
#include "keymap.h"
#include "net.h"
#include "video.h"
#include "updates/server.h"
#include "proxy/server.h"
#include "locate/responder.h"
#include "proxy/access.h"
#include "diagnostics/metrics.h"
#include "diagnostics/window_fit.h"
#include "runtime/tray.h"
#include "runtime/profile.h"
#include "runtime/workspace.h"
#include "../shared/app_icon.h"

#include <imm.h>
#include <shlobj.h>
#include <windowsx.h>

#include <deque>
#include <memory>
#include <set>
#include <thread>

namespace {

constexpr UINT WM_APP_CURSOR = WM_APP + 1;     // lParam = HCURSOR
constexpr UINT WM_APP_CLIPBOARD = WM_APP + 2;  // lParam = std::wstring*
constexpr UINT WM_APP_STATUS = WM_APP + 3;     // lParam = std::wstring*
constexpr UINT WM_APP_WINKEY = WM_APP + 4;     // wParam = down, lParam = 是否右 Win
constexpr UINT WM_APP_TOGGLE_OVERLAY = WM_APP + 5;     // 切换无损层显示（Ctrl+Alt+L 或外部诊断）
constexpr UINT WM_APP_TOGGLE_FULLSCREEN = WM_APP + 6;  // 切换全屏（外部诊断用，等同 Ctrl+Alt+Enter）
constexpr UINT WM_APP_STREAM_SIZE = WM_APP + 7;
constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kViewSizeTimer = 2;  // 窗口大小停止变化后再通知 Windows

struct Settings {
    std::wstring host = L"listen";  // listen 等待被控端；auto 自动发现；其他值为固定地址
    std::wstring lastHost;        // 上次自动发现并连上的地址（发现失败时的后备）
    std::vector<std::wstring> knownHosts;  // 发现过的 Windows 地址，下次直接单播询问
    std::wstring bindHost = L"0.0.0.0";
    uint16_t port = 7400;
    std::string token;
};

HWND g_window = nullptr;
Settings g_settings;
std::mutex g_hostMutex;
std::wstring g_connectedHost;  // 当前连接目标

bool AutoHost() { return g_settings.host.empty() || _wcsicmp(g_settings.host.c_str(), L"auto") == 0; }
VideoPipeline g_video;
StreamDiagnostics g_diagnostics;
std::deque<std::pair<int64_t, int64_t>> g_clockSamples;
std::mutex g_connectionMutex;
std::shared_ptr<Connection> g_connection;
std::atomic<bool> g_running{true};
std::thread g_networkThread;
NetworkAccess g_networkAccess;
ControllerTray g_controllerTray;
ControllerProfile g_profile;
ViewerStartup g_startup;
std::atomic<uint64_t> g_bytesReceived{0};
std::atomic<int64_t> g_rttMicros{-1};
std::atomic<int> g_fps{0};
ServerInfo g_serverInfo;

std::wstring g_status = L"正在启动…";
HCURSOR g_remoteCursor = nullptr;
HHOOK g_keyboardHook = nullptr;
std::set<int> g_pressedRemoteKeys;
int g_buttonsDown = 0;
uint16_t g_lastX = 0, g_lastY = 0;
bool g_fullscreen = false;
int g_streamWidth = 0, g_streamHeight = 0;
WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};
DWORD g_ownClipboardSequence = 0;
std::string g_lastClipboard;
uint64_t g_lastStatsMicros = 0;
bool g_statsTick = false;
std::atomic<int> g_sentViewWidth{0};
std::atomic<int> g_sentViewHeight{0};
// 协议保留显示器模式通知；首版被控端不据此修改物理屏幕或创建虚拟屏。
std::atomic<uint64_t> g_sentMonitor{0};
// 保留原控制端的码率状态接收字段，首版 Windows 被控端尚未发送。
std::atomic<bool> g_wireless{false};
std::atomic<int> g_rateKbps{0};
std::atomic<int> g_rateFps{0};
std::atomic<int> g_queueMs{0};

// 客户区物理像素尺寸；被控端首版按实际主屏和配置上限输出。
void ClientSize(int& width, int& height) {
    RECT client{};
    GetClientRect(g_window, &client);
    width = std::max<LONG>(client.right - client.left, 1);
    height = std::max<LONG>(client.bottom - client.top, 1);
}

// 窗口所在显示器的当前模式，取不到时各项为 0。
void MonitorMode(int& width, int& height, int& refresh) {
    width = height = refresh = 0;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof monitor;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        width = static_cast<int>(mode.dmPelsWidth);
        height = static_cast<int>(mode.dmPelsHeight);
        refresh = static_cast<int>(mode.dmDisplayFrequency);
    }
}

uint64_t MonitorKey(int width, int height, int refresh) {
    return static_cast<uint64_t>(width & 0xFFFF) | static_cast<uint64_t>(height & 0xFFFF) << 16 |
           static_cast<uint64_t>(refresh & 0xFFFF) << 32;
}

std::shared_ptr<Connection> CurrentConnection() {
    std::lock_guard lock(g_connectionMutex);
    return g_connection;
}

bool Connected() { return CurrentConnection() != nullptr; }

void SendToHost(uint8_t type, const Writer& writer) {
    if (auto connection = CurrentConnection()) {
        connection->Send(type, writer.bytes.data(), writer.bytes.size());
    }
}

void PostStatus(const std::wstring& text) {
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g_window, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

// ---------------- 配置 ----------------

std::wstring ConfigPath() {
    if (!g_startup.config.empty()) return g_startup.config;
    if (std::wstring portable = PortableDir(); !portable.empty()) {
        return portable + L"\\client.ini";
    }
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\WindowsStream";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\client.ini";
    }
    CoTaskMemFree(base);
    return path;
}

void LoadSettings() {
    std::wstring path = ConfigPath();
    wchar_t buffer[512];
    GetPrivateProfileStringW(L"connection", L"host", g_settings.host.c_str(), buffer, 512, path.c_str());
    g_settings.host = buffer;
    GetPrivateProfileStringW(L"connection", L"bind_host", g_settings.bindHost.c_str(), buffer, 512, path.c_str());
    g_settings.bindHost = buffer;
    g_settings.port = static_cast<uint16_t>(GetPrivateProfileIntW(L"connection", L"port", g_settings.port, path.c_str()));
    GetPrivateProfileStringW(L"connection", L"last_host", L"", buffer, 512, path.c_str());
    g_settings.lastHost = buffer;
    GetPrivateProfileStringW(L"connection", L"known_hosts", L"", buffer, 512, path.c_str());
    for (std::wstring list = buffer; !list.empty();) {
        size_t comma = list.find(L',');
        std::wstring item = list.substr(0, comma);
        if (!item.empty()) g_settings.knownHosts.push_back(item);
        list = comma == std::wstring::npos ? L"" : list.substr(comma + 1);
    }
    // 配置文件可能正被改写（例如记录上次连接的地址），读不到 token 时稍等重试。
    for (int attempt = 0; attempt < 10; ++attempt) {
        GetPrivateProfileStringW(L"connection", L"token", L"", buffer, 512, path.c_str());
        g_settings.token = WideToUtf8(buffer);
        if (!g_settings.token.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        Sleep(100);
    }
}

void SaveLastHost(const std::wstring& host) {
    if (host == g_settings.lastHost) return;
    g_settings.lastHost = host;
    WritePrivateProfileStringW(L"connection", L"last_host", host.c_str(), ConfigPath().c_str());
}

struct Candidate {
    std::wstring host;
    uint16_t port;
};

void RememberHosts(const std::vector<DiscoveredHost>& found) {
    bool changed = false;
    for (const auto& mac : found) {
        if (std::find(g_settings.knownHosts.begin(), g_settings.knownHosts.end(), mac.address) == g_settings.knownHosts.end()) {
            g_settings.knownHosts.insert(g_settings.knownHosts.begin(), mac.address);
            changed = true;
        }
    }
    if (!changed) return;
    if (g_settings.knownHosts.size() > 8) g_settings.knownHosts.resize(8);
    std::wstring list;
    for (const auto& host : g_settings.knownHosts) list += (list.empty() ? L"" : L",") + host;
    WritePrivateProfileStringW(L"connection", L"known_hosts", list.c_str(), ConfigPath().c_str());
}

// 要尝试连接的地址：固定地址直接用；auto 时局域网发现（有线优先），找不到再试上次成功的地址。
std::vector<Candidate> ConnectionCandidates() {
    if (!AutoHost()) return {{g_settings.host, g_settings.port}};
    PostStatus(L"正在局域网中查找 Windows…");
    std::vector<std::wstring> known = g_settings.knownHosts;
    if (!g_settings.lastHost.empty() && std::find(known.begin(), known.end(), g_settings.lastHost) == known.end()) {
        known.push_back(g_settings.lastHost);
    }
    auto found = DiscoverHosts(g_settings.token, known, 700);
    RememberHosts(found);
    std::vector<Candidate> candidates;
    for (const auto& mac : found) {
        LogLine("发现 %s：%s:%u（%s，%d ms）", WideToUtf8(mac.name).c_str(), WideToUtf8(mac.address).c_str(), mac.port,
                mac.wireless ? "无线" : "有线", mac.replyMs);
        candidates.push_back({mac.address, mac.port});
    }
    if (!g_settings.lastHost.empty() &&
        std::none_of(candidates.begin(), candidates.end(), [](const Candidate& c) { return c.host == g_settings.lastHost; })) {
        candidates.push_back({g_settings.lastHost, g_settings.port});
    }
    return candidates;
}

// ---------------- 网络线程 ----------------

HCURSOR CreateRemoteCursor(const std::vector<uint8_t>& payload) {
    if (payload.size() < 8) return nullptr;
    int w = ReadU16(payload.data()), h = ReadU16(payload.data() + 2);
    int hotX = ReadU16(payload.data() + 4), hotY = ReadU16(payload.data() + 6);
    if (w <= 0 || h <= 0 || payload.size() < 8 + static_cast<size_t>(w) * h * 4) return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = h;  // 自下而上
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    // Windows 给的是预乘 alpha，图标需要非预乘。
    const uint8_t* src = payload.data() + 8;
    auto* dst = static_cast<uint8_t*>(bits);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * w * 4;
        uint8_t* out = dst + static_cast<size_t>(h - 1 - y) * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t a = row[x * 4 + 3];
            for (int c = 0; c < 3; ++c) {
                out[x * 4 + c] = a ? static_cast<uint8_t>(std::min(255, row[x * 4 + c] * 255 / a)) : 0;
            }
            out[x * 4 + 3] = a;
        }
    }
    std::vector<uint8_t> maskBits(static_cast<size_t>((w + 15) / 16 * 2) * h, 0);
    HBITMAP mask = CreateBitmap(w, h, 1, 1, maskBits.data());
    ICONINFO icon{};
    icon.fIcon = FALSE;
    icon.xHotspot = static_cast<DWORD>(hotX);
    icon.yHotspot = static_cast<DWORD>(hotY);
    icon.hbmMask = mask;
    icon.hbmColor = color;
    HCURSOR cursor = CreateIconIndirect(&icon);
    DeleteObject(color);
    DeleteObject(mask);
    return cursor;
}

void HandlePong(const std::vector<uint8_t>& payload) {
    if (payload.size() < 16) return;
    auto& samples = g_clockSamples;  // 每次连接独立估算，避免沿用上一台电脑的时钟。
    uint64_t sent = ReadU64(payload.data());
    uint64_t server = ReadU64(payload.data() + 8);
    uint64_t received = NowMicros();
    int64_t rtt = static_cast<int64_t>(received - sent);
    int64_t offset = static_cast<int64_t>(server) - static_cast<int64_t>((sent + received) / 2);
    samples.emplace_back(rtt, offset);
    if (samples.size() > 16) samples.pop_front();
    auto best = *std::min_element(samples.begin(), samples.end());  // 往返最短的一次时钟差最准
    g_video.clockOffsetMicros = best.second;
    g_video.clockOffsetValid = true;
    g_rttMicros = rtt;
}

void NetworkLoop() {
    while (g_running) {
        auto candidates = ConnectionCandidates();
        if (candidates.empty()) {
            PostStatus(L"未找到被控端");
            for (int i = 0; i < 15 && g_running; ++i) Sleep(100);
            continue;
        }
        std::shared_ptr<Connection> connection;
        ServerInfo info;
        std::wstring error;
        int viewW = 0, viewH = 0;
        ClientSize(viewW, viewH);
        ClientDisplay display;
        display.viewWidth = static_cast<uint16_t>(viewW);
        display.viewHeight = static_cast<uint16_t>(viewH);
        int monitorW = 0, monitorH = 0, refresh = 0;
        MonitorMode(monitorW, monitorH, refresh);
        static uint64_t loggedMonitor = 0;  // 等待重连时每秒循环，显示器不变就不重复记录
        if (MonitorKey(monitorW, monitorH, refresh) != loggedMonitor) {
            loggedMonitor = MonitorKey(monitorW, monitorH, refresh);
            LogLine("控制端显示器：%dx%d，%d Hz", monitorW, monitorH, refresh);
        }
        display.monitorWidth = static_cast<uint16_t>(monitorW);
        display.monitorHeight = static_cast<uint16_t>(monitorH);
        display.refresh = static_cast<uint16_t>(refresh);
        std::wstring host;
        static std::wstring lastError;  // 同一原因持续失败时只记录一次，恢复后重新记录
        for (const auto& candidate : candidates) {
            PostStatus(_wcsicmp(candidate.host.c_str(), L"listen") == 0 ? L"等待被控端连接" : L"正在连接");
            auto attempt = std::make_shared<Connection>();
            const bool reverse = _wcsicmp(candidate.host.c_str(), L"listen") == 0;
            const bool success = reverse
                ? attempt->AcceptReverse(g_settings.bindHost, candidate.port, g_settings.token, display, info, error)
                : attempt->Connect(candidate.host, candidate.port, g_settings.token, display, info, error);
            if (success) {
                connection = attempt;
                host = candidate.host;
                break;
            }
            if (!error.empty() && error != lastError) LogLine("连接失败：%s", WideToUtf8(error).c_str());
            if (!error.empty()) lastError = error;
        }
        if (connection) lastError.clear();
        if (!connection) {
            if (!error.empty()) PostStatus(error);
            for (int i = 0; i < 10 && g_running; ++i) Sleep(100);
            continue;
        }
        if (AutoHost()) SaveLastHost(host);
        {
            std::lock_guard lock(g_hostMutex);
            g_connectedHost = host;
        }
        LogLine("已连接 %s，%dx%d @ %d fps", WideToUtf8(host).c_str(), info.width, info.height, info.fps);
        g_clockSamples.clear(); g_rttMicros = -1; g_video.clockOffsetValid = false;
        g_video.TakeStats();
        g_diagnostics.Begin(info.width, info.height, info.fps);
        PostMessageW(g_window, WM_APP_STREAM_SIZE, info.width, info.height);
        g_serverInfo = info;
        g_wireless = connection->Wireless();
        g_rateKbps = 0;
        LogLine("连接方式：%s", g_wireless ? "无线" : "有线");
        g_sentViewWidth = viewW;
        g_sentViewHeight = viewH;
        g_sentMonitor = MonitorKey(monitorW, monitorH, refresh);
        g_video.ResetOverlay();
        {
            std::lock_guard lock(g_connectionMutex);
            g_connection = connection;
        }
        PostStatus(L"");
        connection->Send(MsgRequestKeyframe);

        uint8_t type = 0;
        std::vector<uint8_t> payload;
        while (g_running && connection->Read(type, payload)) {
            g_bytesReceived += payload.size() + 5;
            g_diagnostics.Received(payload.size() + 5, type == MsgVideo);
            switch (type) {
                case MsgVideo:
                    if (payload.size() > 9) {
                        EncodedFrame frame;
                        frame.captureMicros = ReadU64(payload.data());
                        uint8_t flags = payload[8];
                        frame.keyframe = (flags & 1) != 0;
                        frame.refine = (flags & 2) != 0;
                        size_t offset = 9;
                        if (flags & 4) {  // 变化图块：u32 序号, u16 图块大小, u16 列数, u16 行数, 位图
                            if (payload.size() < offset + 10) break;
                            frame.hasDirty = true;
                            frame.dirtySerial = ReadU32(payload.data() + offset);
                            frame.tileSize = ReadU16(payload.data() + offset + 4);
                            frame.tilesX = ReadU16(payload.data() + offset + 6);
                            frame.tilesY = ReadU16(payload.data() + offset + 8);
                            offset += 10;
                            size_t bitBytes = (static_cast<size_t>(frame.tilesX) * frame.tilesY + 7) / 8;
                            if (payload.size() < offset + bitBytes) break;
                            frame.dirtyBits.assign(payload.begin() + offset, payload.begin() + offset + bitBytes);
                            offset += bitBytes;
                        }
                        frame.data.assign(payload.begin() + offset, payload.end());
                        g_video.Submit(std::move(frame));
                    }
                    break;
                case MsgLossless:
                    g_video.SubmitLossless(std::move(payload));
                    payload = {};
                    break;
                case MsgCursor:
                    if (HCURSOR cursor = CreateRemoteCursor(payload)) {
                        if (!PostMessageW(g_window, WM_APP_CURSOR, 0, reinterpret_cast<LPARAM>(cursor))) {
                            DestroyCursor(cursor);
                        }
                    }
                    break;
                case MsgPong:
                    HandlePong(payload);
                    break;
                case MsgStats:  // u32 码率 kbps, u16 帧率上限, u16 排队 ms, u32 链路 kbps
                    if (payload.size() >= 8) {
                        g_rateKbps = static_cast<int>(ReadU32(payload.data()));
                        g_rateFps = ReadU16(payload.data() + 4);
                        g_queueMs = ReadU16(payload.data() + 6);
                    }
                    break;
                case MsgHostInfo:
                    if (payload.size() <= 4096) {
                        const std::string text(payload.begin(), payload.end());
                        LogLine("被控端：%s", text.c_str());
                    }
                    break;
                case MsgTelemetry:
                    g_diagnostics.Remote(payload);
                    break;
                case MsgClipboard: {
                    auto* text = new std::wstring(Utf8ToWide(std::string(payload.begin(), payload.end())));
                    if (!PostMessageW(g_window, WM_APP_CLIPBOARD, 0, reinterpret_cast<LPARAM>(text))) {
                        delete text;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        {
            std::lock_guard lock(g_connectionMutex);
            g_connection.reset();
        }
        g_video.clockOffsetValid = false;
        g_diagnostics.End();
        LogLine("连接断开");
        if (g_running) {
            PostStatus(L"连接已断开，正在重连…");
            Sleep(300);
        }
    }
}

// ---------------- 输入 ----------------

bool MapPoint(int x, int y, uint16_t& nx, uint16_t& ny) {
    int frameW = g_video.FrameWidth(), frameH = g_video.FrameHeight();
    RECT client{};
    GetClientRect(g_window, &client);
    Rect fit = VideoRect(client.right, client.bottom, frameW, frameH);
    if (fit.w <= 0 || fit.h <= 0) return false;
    double fx = std::clamp((x - fit.x + 0.5) / fit.w, 0.0, 1.0);
    double fy = std::clamp((y - fit.y + 0.5) / fit.h, 0.0, 1.0);
    nx = static_cast<uint16_t>(fx * 65535 + 0.5);
    ny = static_cast<uint16_t>(fy * 65535 + 0.5);
    return true;
}

void SendKey(int remoteKey, bool down) {
    if (down) {
        g_pressedRemoteKeys.insert(remoteKey);
    } else {
        g_pressedRemoteKeys.erase(remoteKey);
    }
    Writer w;
    w.u16(static_cast<uint16_t>(remoteKey));
    w.u8(down ? 1 : 0);
    SendToHost(MsgKey, w);
}

void SendButton(int button, bool down, LPARAM lParam) {
    uint16_t x = g_lastX, y = g_lastY;
    MapPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y);
    g_lastX = x;
    g_lastY = y;
    if (down) {
        if (g_buttonsDown == 0) SetCapture(g_window);
        g_buttonsDown |= 1 << button;
    } else {
        g_buttonsDown &= ~(1 << button);
        if (g_buttonsDown == 0) ReleaseCapture();
    }
    Writer w;
    w.u8(static_cast<uint8_t>(button));
    w.u8(down ? 1 : 0);
    w.u16(x);
    w.u16(y);
    SendToHost(MsgMouseButton, w);
}

void SendScroll(int dx, int dy) {
    Writer w;
    w.u16(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(dx, -32768, 32767))));
    w.u16(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(dy, -32768, 32767))));
    SendToHost(MsgScroll, w);
}

// 失去焦点或切换时松开 Windows 上所有按键和鼠标键，避免卡键。
void ReleaseAllRemote() {
    if (!g_pressedRemoteKeys.empty() || g_buttonsDown != 0) {
        SendToHost(MsgReleaseAll, Writer{});
    }
    g_pressedRemoteKeys.clear();
    if (g_buttonsDown != 0) {
        g_buttonsDown = 0;
        ReleaseCapture();
    }
}

// 窗口在前台且已连接时截获 Win 键，避免弹出开始菜单，转发给 Windows。
LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if ((info->vkCode == VK_LWIN || info->vkCode == VK_RWIN) && ViewerHasFocus(g_window) && Connected()) {
            bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            PostMessageW(g_window, WM_APP_WINKEY, down, info->vkCode == VK_RWIN);
            return 1;
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

// ---------------- 剪贴板 ----------------

std::wstring ReadClipboardText() {
    std::wstring text;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(g_window)) {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
                    text = p;
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(10);
    }
    return text;
}

std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to) {
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::wstring::npos; pos += to.size()) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

void OnLocalClipboardChanged() {
    if (!ViewerHasFocus(g_window)) return;
    if (!g_profile.clipboard) return;
    if (GetClipboardSequenceNumber() == g_ownClipboardSequence || !Connected()) return;
    std::string utf8 = WideToUtf8(ReplaceAll(ReadClipboardText(), L"\r\n", L"\n"));
    if (utf8.empty() || utf8 == g_lastClipboard || utf8.size() > 4 * 1024 * 1024) return;
    g_lastClipboard = utf8;
    Writer w;
    w.append(utf8.data(), utf8.size());
    SendToHost(MsgClipboard, w);
}

void ApplyRemoteClipboard(const std::wstring& text) {
    if (!ViewerHasFocus(g_window)) return;
    if (!g_profile.clipboard) return;
    std::string utf8 = WideToUtf8(text);
    if (utf8 == g_lastClipboard) return;
    g_lastClipboard = utf8;
    std::wstring windowsText = ReplaceAll(ReplaceAll(text, L"\r\n", L"\n"), L"\n", L"\r\n");
    size_t bytes = (windowsText.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return; }
    memcpy(destination, windowsText.c_str(), bytes);
    GlobalUnlock(memory);
    if (OpenClipboard(g_window)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
        g_ownClipboardSequence = GetClipboardSequenceNumber();
    } else {
        GlobalFree(memory);
    }
}

// ---------------- 窗口 ----------------

void ToggleFullscreen() {
    ReleaseAllRemote();
    if (GetWindowLongPtrW(g_window, GWL_STYLE) & WS_CHILD) {
        PostMessageW(GetAncestor(g_window, GA_ROOT), kWorkspaceAction, kWorkspaceFullscreen, 0);
        return;
    }
    LONG style = GetWindowLongW(g_window, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO monitor{sizeof monitor};
        if (GetWindowPlacement(g_window, &g_placement) &&
            GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            SetWindowLongW(g_window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                         monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongW(g_window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_window, &g_placement);
        SetWindowPos(g_window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

void UpdateTitle() {
    std::wstring title = g_profile.title;
    if (!g_status.empty()) title += L" · " + g_status;
    else if (Connected()) title += L" · " + g_diagnostics.Title();
    SetWindowTextW(g_window, title.c_str());
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) ReleaseAllRemote();
    if (g_controllerTray.Handle(message, wParam, lParam)) return 0;
    if (g_networkAccess.Handle(message, wParam)) return 0;
    switch (message) {
        case WM_SIZE:
            FitStreamWindow(hwnd, g_streamWidth, g_streamHeight);
            g_video.RequestResize();
            SetTimer(hwnd, kViewSizeTimer, 250, nullptr);
            return 0;
        case WM_MOVE:            // 可能拖到了另一块显示器
        case WM_DISPLAYCHANGE:   // 分辨率、刷新率或显示器变了
            SetTimer(hwnd, kViewSizeTimer, 250, nullptr);
            break;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            g_video.RequestRedraw();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(g_remoteCursor && Connected() ? g_remoteCursor : LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE: {
            uint16_t x, y;
            if (Connected() && MapPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y) && (x != g_lastX || y != g_lastY)) {
                g_lastX = x;
                g_lastY = y;
                Writer w;
                w.u16(x);
                w.u16(y);
                SendToHost(MsgMouseMove, w);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: SendButton(0, true, lParam); return 0;
        case WM_LBUTTONUP: SendButton(0, false, lParam); return 0;
        case WM_RBUTTONDOWN: SendButton(1, true, lParam); return 0;
        case WM_RBUTTONUP: SendButton(1, false, lParam); return 0;
        case WM_MBUTTONDOWN: SendButton(2, true, lParam); return 0;
        case WM_MBUTTONUP: SendButton(2, false, lParam); return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            SendButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4, message == WM_XBUTTONDOWN, lParam);
            return TRUE;
        case WM_MOUSEWHEEL:
            SendScroll(0, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_MOUSEHWHEEL:
            SendScroll(GET_WHEEL_DELTA_WPARAM(wParam), 0);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            UINT vk = static_cast<UINT>(wParam);
            if (HandleWorkspaceShortcut(hwnd, message, wParam)) { ReleaseAllRemote(); return 0; }
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (down && vk == VK_RETURN && ctrl && alt) {
                ToggleFullscreen();
                return 0;
            }
            if (down && vk == 'L' && ctrl && alt) {
                PostMessageW(hwnd, WM_APP_TOGGLE_OVERLAY, 0, 0);
                return 0;
            }
            if (down && vk == VK_F4 && alt) {
                ReleaseAllRemote();  // Alt+F4 留给 Windows 关闭窗口
                break;
            }
            if (!Connected()) break;
            UINT scan = (lParam >> 16) & 0xFF;
            bool extended = (lParam >> 24) & 1;
            if (scan == 0) scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            int remoteKey = WindowsKeyFromScanCode(scan, extended, vk);
            if (remoteKey >= 0) SendKey(remoteKey, down);
            return 0;
        }
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return 0;
        case WM_APP_WINKEY: {
            int remoteKey = WindowsKeyFromScanCode(lParam ? 0x5C : 0x5B, true);
            bool down = wParam != 0;
            if (!down || !g_pressedRemoteKeys.count(remoteKey)) SendKey(remoteKey, down);
            return 0;
        }
        case WM_APP_TOGGLE_FULLSCREEN:
            ToggleFullscreen();
            return 0;
        case WM_APP_STREAM_SIZE:
            g_streamWidth = static_cast<int>(wParam); g_streamHeight = static_cast<int>(lParam);
            FitStreamWindow(hwnd, g_streamWidth, g_streamHeight);
            return 0;
        case WM_APP_TOGGLE_OVERLAY:
            g_video.overlayEnabled = !g_video.overlayEnabled;
            g_video.RequestRedraw();
            LogLine("无损层显示：%s", g_video.overlayEnabled ? "开" : "关");
            return 0;
        case WM_KILLFOCUS:
            ReleaseAllRemote();
            return 0;

        case WM_CLIPBOARDUPDATE:
            OnLocalClipboardChanged();
            return 0;
        case WM_APP_CLIPBOARD: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            ApplyRemoteClipboard(*text);
            return 0;
        }
        case WM_APP_CURSOR: {
            if (g_remoteCursor) DestroyCursor(g_remoteCursor);
            g_remoteCursor = reinterpret_cast<HCURSOR>(lParam);
            POINT p;
            RECT client;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            GetClientRect(hwnd, &client);
            if (PtInRect(&client, p) && ViewerHasFocus(hwnd)) SetCursor(g_remoteCursor);
            return 0;
        }
        case WM_APP_STATUS: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            g_status = *text;
            UpdateTitle();
            return 0;
        }
        case kViewerFocus:
            SetFocus(hwnd);
            OnLocalClipboardChanged();
            return 0;
        case kViewerSecureAttention:
            if (GetPrivateProfileIntW(L"permissions", L"secure_attention", 0, ConfigPath().c_str()))
                if (const auto connection = CurrentConnection()) connection->Send(MsgSecureAttention);
            return 0;
        case WM_TIMER:
            if (wParam == kViewSizeTimer) {
                KillTimer(hwnd, kViewSizeTimer);
                int monitorW = 0, monitorH = 0, refresh = 0;
                MonitorMode(monitorW, monitorH, refresh);
                uint64_t monitorKey = MonitorKey(monitorW, monitorH, refresh);
                if (refresh > 0 && Connected() && monitorKey != g_sentMonitor) {
                    g_sentMonitor = monitorKey;
                    Writer w;
                    w.u16(static_cast<uint16_t>(monitorW));
                    w.u16(static_cast<uint16_t>(monitorH));
                    w.u16(static_cast<uint16_t>(refresh));
                    SendToHost(MsgMonitorMode, w);
                    LogLine("显示器变为 %dx%d @ %d Hz", monitorW, monitorH, refresh);
                }
                int width = 0, height = 0;
                ClientSize(width, height);
                if (!IsIconic(hwnd) && Connected() && (width != g_sentViewWidth || height != g_sentViewHeight)) {
                    g_sentViewWidth = width;
                    g_sentViewHeight = height;
                    Writer w;
                    w.u16(static_cast<uint16_t>(width));
                    w.u16(static_cast<uint16_t>(height));
                    SendToHost(MsgViewSize, w);
                    LogLine("显示区域变为 %dx%d", width, height);
                }
                return 0;
            }
            if (wParam == kTimer) {
                g_networkAccess.Tick(Connected());
                Writer w;
                w.u64(NowMicros());
                SendToHost(MsgPing, w);
                g_statsTick = !g_statsTick;
                if (g_statsTick) {
                    DisplayMetrics display;
                    MonitorMode(display.width, display.height, display.refreshHz);
                    ClientSize(display.viewWidth, display.viewHeight);
                    g_diagnostics.Tick(g_video.TakeStats(), g_rttMicros.load(), display);
                    UpdateTitle();
                }
            }
            return 0;

        case WM_DESTROY:
            g_networkAccess.Stop();
            g_running = false;
            if (auto connection = CurrentConnection()) connection->Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const int guardResult = proxy::RunProxyGuard();
    if (guardResult >= 0) return guardResult;
    const int workspaceResult = RunControllerWorkspace(instance, show, g_startup);
    if (workspaceResult >= 0) return workspaceResult;
    // 同一连接配置仍保持单实例，独立配置可以打开不同服务器窗口。
    g_profile = LoadControllerProfile(ConfigPath(), !g_startup.primary);
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, g_profile.mutex.c_str());
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(g_profile.windowClass.c_str(), nullptr)) {
            ShowWindowAsync(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing);
            LogLine("控制端已经运行，已请求显示现有窗口");
        }
        CloseHandle(singleInstance);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    LoadSettings();
    UpdateServer updateServer;
    ProxyServer proxyServer;
    if (g_startup.primary) {
        updateServer.StartConfigured(ConfigPath(), g_settings.bindHost, g_settings.token);
        proxyServer.StartConfigured(ConfigPath(), g_settings.bindHost, g_settings.token);
    }
    LogLine("WindowsStream 客户端启动，目标 %s:%u", WideToUtf8(g_settings.host).c_str(), g_settings.port);
    LocateResponder locateResponder;
    if (g_startup.primary && _wcsicmp(g_settings.host.c_str(), L"listen") == 0) locateResponder.Start(g_settings.bindHost, g_settings.port, g_settings.token);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadAppIcon();
    wc.hIconSm = LoadAppIcon(true);
    wc.lpszClassName = g_profile.windowClass.c_str();
    RegisterClassExW(&wc);

    // 初始客户区上限为 1920×1080，超出本机工作区时按比例缩小。
    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{sizeof monitorInfo};
    GetMonitorInfoW(monitor, &monitorInfo);
    UINT dpi = 96;
    if (HDC dc = GetDC(nullptr)) {
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(nullptr, dc);
    }
    int workW = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    int workH = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    int clientW = 1920, clientH = 1080;
    RECT frame = {0, 0, clientW, clientH};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    if (frame.right - frame.left > workW || frame.bottom - frame.top > workH) {
        Rect fit = VideoRect(workW * 9 / 10, workH * 9 / 10, 16, 9);
        frame = {0, 0, fit.w, fit.h};
        AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    }
    int windowW = frame.right - frame.left, windowH = frame.bottom - frame.top;
    const bool hosted = g_startup.workspace && IsWindow(g_startup.workspace);
    g_window = CreateWindowExW(0, wc.lpszClassName, g_profile.title.c_str(), hosted ? WS_CHILD | WS_CLIPSIBLINGS : WS_OVERLAPPEDWINDOW,
                               monitorInfo.rcWork.left + (workW - windowW) / 2, monitorInfo.rcWork.top + (workH - windowH) / 2,
                               windowW, windowH, hosted ? g_startup.workspace : nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    if (g_startup.primary) g_networkAccess.Start(g_window, ConfigPath(), proxyServer);
    ImmAssociateContextEx(g_window, nullptr, 0);  // 关闭输入法，按键原样发给 Windows

    if (!g_video.Initialize(g_window)) {
        MessageBoxW(g_window, L"无法初始化硬件解码或 D3D11，请查看 client.log", L"WindowsStream", MB_ICONERROR);
        return 1;
    }
    g_video.onDecodeError = [] {
        static std::atomic<uint64_t> last{0};
        uint64_t now = NowMicros();
        if (now - last.load() > 300'000) {
            last = now;
            if (auto connection = CurrentConnection()) connection->Send(MsgRequestKeyframe);
        }
    };

    if (g_startup.primary) g_controllerTray.Start(g_window);
    AddClipboardFormatListener(g_window);
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, instance, 0);
    SetTimer(g_window, kTimer, 500, nullptr);
    if (!hosted) ShowWindow(g_window, show == SW_HIDE ? SW_SHOWNORMAL : show);

    if (g_settings.token.size() < 32) {
        g_status = L"请配置连接口令";
        LogLine("连接口令至少需要 32 个字节，请检查配置文件");
        UpdateTitle();
    } else {
        g_networkThread = std::thread(NetworkLoop);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    g_networkAccess.Stop();
    if (auto connection = CurrentConnection()) connection->Shutdown();
    if (g_networkThread.joinable()) g_networkThread.join();
    Connection::CloseReverseListener();
    g_video.Shutdown();
    updateServer.Stop();
    proxyServer.Stop();
    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    WSACleanup();
    CoUninitialize();
    if (singleInstance) CloseHandle(singleInstance);
    return 0;
}
