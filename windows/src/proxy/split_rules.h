#pragma once
#include "system_settings.h"

namespace proxy {
// 生成系统 PAC；只把匹配的内网地址送到 B，其余采用启用前的直连/手动代理。
bool BuildSplitPac(const std::wstring& ini, uint16_t port, const SystemSettings& original, std::string& script);
}
