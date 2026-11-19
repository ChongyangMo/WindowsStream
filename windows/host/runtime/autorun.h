#pragma once
#include "../../src/common.h"
#include <filesystem>

inline constexpr wchar_t kRelayAutoStartArgument[] = L"--relay-autostart";

bool SetUserAutoStart(bool enabled, bool relay);
bool SaveUserAutoStart(bool enabled, bool relay, const std::wstring& ini);
bool IsAdminPackageRuntime(const std::filesystem::path& root);
int LaunchRelayAutoStart();
