#pragma once
#include "../config.h"
#include <array>

class HostView {
public:
    ~HostView();
    bool Create(HWND window, const HostConfig& config);
    void ResizeForDpi(UINT dpi, const RECT* suggested = nullptr);
    void SetAddress(const std::wstring& address);
    void SetStatus(const std::wstring& status);
    void SetColors();
    void Paint() const;
    HBRUSH ControlColor(HDC dc, HWND control) const;
    static RECT WindowBounds(DWORD style, UINT dpi);
private:
    enum Control : size_t {
        Status, StatusHint, Mode, AddressLabel, Address, VersionLabel, Version, PermissionLabel, Permission,
        ClipboardLabel, ClipboardValue, DragLabel, DragValue, Hide, Exit, Count
    };
    int Scale(int value) const;
    void Layout();
    void LayoutStatus();
    HWND window_ = nullptr;
    std::array<HWND, Count> controls_{};
    UINT dpi_ = 96;
    HFONT body_ = nullptr, value_ = nullptr, headline_ = nullptr;
    HBRUSH canvas_ = nullptr, card_ = nullptr, badge_ = nullptr;
    COLORREF text_{}, secondary_{}, border_{}, accent_{}, success_{}, idle_{};
    bool connected_ = false, exclusive_ = false, clipboard_ = false, drag_ = false;
};
