#pragma once
#include "catalog.h"
#include "picker.h"
#include "../workspace.h"
#include <commctrl.h>

namespace workspace {
struct Tab {
    Profile profile;
    HWND view = nullptr;
    HANDLE process = nullptr;
    DWORD processId = 0;
    bool borrowed = false;
    LONG_PTR style = 0, exStyle = 0;
    HMENU menu = nullptr;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    ULONGLONG started = 0;
};

class Window {
public:
    explicit Window(std::wstring base) : base_(std::move(base)) {}
    HWND Create(int show);
    void Open(const Profile& profile);
    bool Translate(MSG& message);
    static LRESULT CALLBACK Keyboard(int code, WPARAM, LPARAM);
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TabProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void NewConnection();
    void Refresh();
    void Layout();
    void Select(int index);
    void CloseTab(int index);
    void Detach(int index);
    void Release(Tab& tab);
    void Attach(Tab& tab);
    void Poll();
    void DrawTab(const DRAWITEMSTRUCT& item);
    void Menu(int index);
    void Fullscreen();
    int HeaderHeight() const;
    HWND window_ = nullptr, tabsControl_ = nullptr, add_ = nullptr;
    std::wstring base_;
    std::vector<Tab> tabs_;
    int selected_ = -1;
    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    ConnectionPicker picker_;
};

extern std::vector<Window*> windows;
constexpr wchar_t kWindowClass[] = L"WindowsStreamWorkspace";
}
