#include "keepalive_task.h"
#include "../../src/common.h"
#include <comutil.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> info(bytes);
    const bool read = bytes && GetTokenInformation(token, TokenUser, info.data(), bytes, &bytes);
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!read || !ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(info.data())->User.Sid, &sid)) return {};
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

std::wstring XmlText(const std::wstring& text) {
    std::wstring escaped;
    for (const auto ch : text) {
        switch (ch) {
            case L'&': escaped += L"&amp;"; break;
            case L'<': escaped += L"&lt;"; break;
            case L'>': escaped += L"&gt;"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

std::wstring TaskXml(const std::wstring& sid, const std::filesystem::path& program, const std::filesystem::path& root) {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t start[32]{};
    swprintf_s(start, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    return std::wstring(LR"(<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
<RegistrationInfo><Description>WindowsStream 被控端保活</Description></RegistrationInfo>
<Triggers>
<LogonTrigger><UserId>)") + sid + LR"(</UserId></LogonTrigger>
<TimeTrigger><Repetition><Interval>PT1M</Interval></Repetition><StartBoundary>)" + start + LR"(</StartBoundary></TimeTrigger>
</Triggers>
<Principals><Principal id="User"><UserId>)" + sid + LR"(</UserId>
<LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>
<Settings>
<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
<StartWhenAvailable>true</StartWhenAvailable>
<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
</Settings>
<Actions Context="User"><Exec><Command>)" + XmlText(program.wstring()) + LR"(</Command><Arguments>)" +
        kScheduledKeepAliveArgument + LR"(</Arguments><WorkingDirectory>)" + XmlText(root.wstring()) +
        LR"(</WorkingDirectory></Exec></Actions></Task>)";
}

bool Matches(IRegisteredTask* task, const std::filesystem::path& program) {
    VARIANT_BOOL enabled = VARIANT_FALSE;
    BSTR xml = nullptr;
    if (FAILED(task->get_Enabled(&enabled)) || enabled != VARIANT_TRUE || FAILED(task->get_Xml(&xml))) return false;
    const _bstr_t definition(xml, false);
    const std::wstring_view contents(static_cast<const wchar_t*>(definition), definition.length());
    return contents.find(L"<Command>" + XmlText(program.wstring()) + L"</Command>") != std::wstring_view::npos &&
        contents.find(L"<Interval>PT1M</Interval>") != std::wstring_view::npos &&
        contents.find(kScheduledKeepAliveArgument) != std::wstring_view::npos;
}
}

HRESULT RegisterKeepAliveTask(const std::filesystem::path& program, const std::filesystem::path& root) {
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) return apartment.result;
    const auto sid = CurrentUserSid();
    if (sid.empty()) return E_ACCESSDENIED;
    const _bstr_t name((L"WindowsStream-HostKeepAlive-" + sid).c_str());
    ComPtr<ITaskService> service;
    auto result = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(result)) return result;
    if (FAILED(result = service->Connect({}, {}, {}, {}))) return result;
    ComPtr<ITaskFolder> folder;
    if (FAILED(result = service->GetFolder(_bstr_t(L"\\"), &folder))) return result;
    ComPtr<IRegisteredTask> task;
    if (FAILED(folder->GetTask(name, &task)) || !Matches(task.Get(), program)) {
        const _bstr_t definition(TaskXml(sid, program, root).c_str());
        result = folder->RegisterTask(name, definition, TASK_CREATE_OR_UPDATE, _variant_t(sid.c_str()), {},
                                      TASK_LOGON_INTERACTIVE_TOKEN, {}, task.ReleaseAndGetAddressOf());
        if (FAILED(result)) return result;
        LogLine("已注册被控端保活计划任务，登录时启动，每分钟补启动一次");
    }
    TASK_STATE state = TASK_STATE_UNKNOWN;
    if (SUCCEEDED(task->get_State(&state)) && state == TASK_STATE_RUNNING) return S_OK;
    ComPtr<IRunningTask> running;
    return task->Run({}, &running);
}
