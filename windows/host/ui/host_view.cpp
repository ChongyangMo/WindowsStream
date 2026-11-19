#include "host_view.h"
#include "../version.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr int kWidth = 432, kHeight = 336;
struct Box { int x, y, width, height; };

HFONT Font(UINT dpi, int size, int weight) {
    return CreateFontW(-MulDiv(size, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
}
}

HostView::~HostView() {
    for (const auto font : {body_, value_, headline_}) if (font) DeleteObject(font);
    for (const auto brush : {canvas_, card_, badge_}) if (brush) DeleteObject(brush);
}

RECT HostView::WindowBounds(DWORD style, UINT dpi) {
    RECT result{0, 0, MulDiv(kWidth, dpi, 96), MulDiv(kHeight, dpi, 96)};
    AdjustWindowRectExForDpi(&result, style, FALSE, 0, dpi);
    return result;
}

bool HostView::Create(HWND window, const HostConfig& config) {
    window_ = window;
    dpi_ = GetDpiForWindow(window_);
    clipboard_ = config.clipboard;
    drag_ = config.fileDrag;
    const auto relay = GetFileAttributesW((PortableDir() + L"\\relay.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
    const wchar_t* mode = relay ? L"中转" : config.reverse ? L"反向直连" : L"直连";
    const wchar_t* labels[] = { L"正在启动", L"Ctrl+Alt+F11 解除", mode, L"本机 IP", L"", L"版号", HostVersion().c_str(),
        L"运行权限", config.elevated ? L"管理员" : L"普通用户",
        L"文字剪贴板", clipboard_ ? L"已启用" : L"已关闭",
        L"双向文件拖拽", drag_ ? L"已启用" : L"已关闭", L"隐藏", L"退出" };
    static_assert(ARRAYSIZE(labels) == Count);
    for (size_t i = 0; i < Count; ++i) {
        const bool button = i == Hide || i == Exit;
        const bool address = i == Address;
        const auto style = button ? BS_PUSHBUTTON : address ? ES_READONLY | ES_AUTOHSCROLL :
            SS_NOPREFIX | (i == Mode ? SS_CENTER : SS_LEFT) | SS_CENTERIMAGE;
        const auto id = button ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(i == Hide ? 1 : 2)) : nullptr;
        controls_[i] = CreateWindowW(button ? L"BUTTON" : address ? L"EDIT" : L"STATIC", labels[i],
            WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_, id, nullptr, nullptr);
        if (!controls_[i]) return false;
    }
    SendMessageW(controls_[Address], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SetColors();
    Layout();
    return true;
}

int HostView::Scale(int value) const { return MulDiv(value, dpi_, 96); }

void HostView::Layout() {
    constexpr Box boxes[] = {
        {58, 29, 230, 54}, {59, 61, 229, 21}, {310, 41, 86, 30},
        {40, 115, 119, 34}, {166, 123, 218, 25},
        {40, 156, 119, 34}, {166, 156, 218, 34},
        {40, 197, 119, 34}, {166, 197, 218, 34},
        {22, 241, 94, 28}, {119, 241, 73, 28},
        {218, 241, 111, 28}, {332, 241, 76, 28},
        {231, 283, 84, 34}, {325, 283, 84, 34},
    };
    static_assert(ARRAYSIZE(boxes) == Count);
    const auto body = Font(dpi_, 15, FW_NORMAL);
    const auto value = Font(dpi_, 16, FW_SEMIBOLD);
    const auto headline = Font(dpi_, 25, FW_SEMIBOLD);
    for (size_t i = 0; i < Count; ++i) {
        const auto& b = boxes[i];
        SetWindowPos(controls_[i], nullptr, Scale(b.x), Scale(b.y), Scale(b.width), Scale(b.height), SWP_NOZORDER | SWP_NOACTIVATE);
        const auto font = i == Status ? headline : i == Address || i == Version || i == Permission ? value : body;
        SendMessageW(controls_[i], WM_SETFONT,
            reinterpret_cast<WPARAM>(font ? font : GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    }
    for (const auto font : {body_, value_, headline_}) if (font) DeleteObject(font);
    body_ = body; value_ = value; headline_ = headline;
    LayoutStatus();
}

void HostView::LayoutStatus() {
    SetWindowPos(controls_[Status], nullptr, Scale(58), Scale(exclusive_ ? 24 : 29),
        Scale(230), Scale(exclusive_ ? 36 : 54), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(controls_[StatusHint], exclusive_ ? SW_SHOWNA : SW_HIDE);
}
void HostView::ResizeForDpi(UINT dpi, const RECT* suggested) {
    const auto size = WindowBounds(static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)), dpi);
    SetWindowPos(window_, nullptr, suggested ? suggested->left : 0, suggested ? suggested->top : 0,
        size.right - size.left, size.bottom - size.top,
        SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));
    if (dpi_ != dpi) {
        dpi_ = dpi;
        Layout();
    }
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void HostView::SetAddress(const std::wstring& address) {
    SetWindowTextW(controls_[Address], address.c_str());
}

void HostView::SetStatus(const std::wstring& status) {
    const bool exclusive = status.starts_with(L"鼠标独占中");
    connected_ = status == L"已连接" || exclusive;
    SetWindowTextW(controls_[Status], exclusive ? L"鼠标独占中" : status.c_str());
    if (exclusive_ != exclusive) {
        exclusive_ = exclusive;
        LayoutStatus();
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void HostView::SetColors() {
    HIGHCONTRASTW contrast{sizeof contrast};
    const bool high = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    for (const auto brush : {canvas_, card_, badge_}) if (brush) DeleteObject(brush);
    canvas_ = CreateSolidBrush(high ? GetSysColor(COLOR_WINDOW) : RGB(243, 246, 250));
    card_ = CreateSolidBrush(high ? GetSysColor(COLOR_WINDOW) : RGB(255, 255, 255));
    badge_ = CreateSolidBrush(high ? GetSysColor(COLOR_WINDOW) : RGB(234, 242, 252));
    text_ = high ? GetSysColor(COLOR_WINDOWTEXT) : RGB(23, 36, 54);
    secondary_ = high ? text_ : RGB(73, 90, 111);
    border_ = high ? text_ : RGB(216, 227, 239);
    accent_ = high ? text_ : RGB(38, 82, 138);
    success_ = high ? text_ : RGB(12, 110, 79);
    idle_ = high ? text_ : RGB(148, 84, 15);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

HBRUSH HostView::ControlColor(HDC dc, HWND control) const {
    const auto cell = static_cast<size_t>(std::find(controls_.begin(), controls_.end(), control) - controls_.begin());
    const bool enabled = (cell == ClipboardValue && clipboard_) || (cell == DragValue && drag_);
    const bool sharing = cell >= ClipboardLabel && cell <= DragValue;
    const bool value = cell == Address || cell == Version || cell == Permission || cell == Status;
    SetTextColor(dc, cell == Mode ? accent_ : enabled ? success_ : value ? text_ : secondary_);
    SetBkMode(dc, TRANSPARENT);
    return cell == Mode ? badge_ : sharing ? canvas_ : card_;
}

void HostView::Paint() const {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window_, &paint);
    FillRect(dc, &paint.rcPaint, canvas_);
    const auto border = CreatePen(PS_SOLID, 1, border_);
    const auto previousPen = SelectObject(dc, border);
    const auto previousBrush = SelectObject(dc, card_);
    RoundRect(dc, Scale(20), Scale(20), Scale(412), Scale(93), Scale(14), Scale(14));
    RoundRect(dc, Scale(20), Scale(106), Scale(412), Scale(232), Scale(14), Scale(14));
    for (const int y : {152, 193}) {
        MoveToEx(dc, Scale(40), Scale(y), nullptr);
        LineTo(dc, Scale(392), Scale(y));
    }
    SelectObject(dc, GetStockObject(NULL_PEN));
    SelectObject(dc, badge_);
    RoundRect(dc, Scale(307), Scale(39), Scale(399), Scale(73), Scale(12), Scale(12));
    const auto dot = CreateSolidBrush(connected_ ? success_ : idle_);
    SelectObject(dc, dot);
    Ellipse(dc, Scale(39), Scale(51), Scale(49), Scale(61));
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(dot);
    DeleteObject(border);
    EndPaint(window_, &paint);
}
