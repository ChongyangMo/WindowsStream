#pragma once
#include "../../src/common.h"
#include <filesystem>

void SetTemporaryLockPolicy(const std::filesystem::path& root, uint64_t created);
bool RestoreLockPolicy(const std::filesystem::path& root, DWORD owner, uint64_t created);
void RestoreDormantLockPolicy(const std::filesystem::path& root);
