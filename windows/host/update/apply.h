#pragma once
#include "../../shared/update_package.h"
#include <filesystem>

namespace updates {
bool Stage(const std::filesystem::path& root, DWORD parent, const Package& package,
           const std::vector<uint8_t>& manifest, const std::string& token);
int Apply(const std::wstring& ticket);
bool BootstrapExisting();
void MarkHealthy(const std::string& bootId);
}
