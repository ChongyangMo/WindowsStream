#pragma once
#include <filesystem>

inline constexpr wchar_t kScheduledKeepAliveArgument[] = L"--scheduled-keepalive";

void StartScheduledKeepAlive(const std::filesystem::path& root, bool elevated);
void PauseScheduledKeepAlive(const std::filesystem::path& root);
int RunScheduledKeepAlive();
