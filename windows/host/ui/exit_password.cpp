#include "../../src/common.h"
#include "exit_password.h"
#include "../../resources/resource.h"

bool HostExitPassword::Confirm(HWND owner) {
    if (dialog_) {
        SetForegroundWindow(dialog_);
        return false;
    }
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_HOST_EXIT_PASSWORD), owner,
        DialogProc, reinterpret_cast<LPARAM>(this));
    const DWORD error = result == -1 ? GetLastError() : ERROR_SUCCESS;
    dialog_ = nullptr;
    if (result == -1) LogLine("无法打开退出密码窗口：%lu", error);
    return result == IDOK;
}

void HostExitPassword::Cancel() {
    if (dialog_) EndDialog(dialog_, IDCANCEL);
}

INT_PTR CALLBACK HostExitPassword::DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG:
            reinterpret_cast<HostExitPassword*>(lParam)->dialog_ = dialog;
            return TRUE;
        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            if (LOWORD(wParam) == IDOK) {
                constexpr wchar_t password[] = L"123456";
                wchar_t entered[_countof(password) + 1]{};
                GetDlgItemTextW(dialog, IDC_HOST_EXIT_PASSWORD, entered, _countof(entered));
                const bool matches = wcscmp(entered, password) == 0;
                SecureZeroMemory(entered, sizeof entered);
                SetDlgItemTextW(dialog, IDC_HOST_EXIT_PASSWORD, L"");
                if (matches) EndDialog(dialog, IDOK);
                else {
                    ShowWindow(GetDlgItem(dialog, IDC_HOST_EXIT_ERROR), SW_SHOW);
                    SetFocus(GetDlgItem(dialog, IDC_HOST_EXIT_PASSWORD));
                }
                return TRUE;
            }
            break;
    }
    return FALSE;
}
