#pragma once
#include "catalog.h"
#include <functional>

namespace workspace {
class ConnectionPicker {
public:
    void Show(HWND owner, const std::wstring& base, std::function<void(const Profile&)> open);
    void Close();
    bool Translate(MSG& message);
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    void Select();
    void Connect();
    HWND window_ = nullptr, list_ = nullptr, host_ = nullptr, port_ = nullptr, token_ = nullptr, error_ = nullptr;
    std::wstring base_;
    std::vector<Profile> profiles_;
    std::function<void(const Profile&)> open_;
};
}
