#pragma once
#include "../../src/common.h"
#include <filesystem>

bool RestoreScreenSaver(const std::filesystem::path& root, DWORD owner, uint64_t created);
void RestoreDormantScreenSaver(const std::filesystem::path& root);

class AwakeLease {
public:
    bool Enable();
    void Refresh();
    void Release(bool preserveScreenSaver = false);
private:
    void DisableScreenSaver();
    void LogPolicy();
    bool enabled_ = false;
    std::filesystem::path root_;
    uint64_t created_ = 0;
};
