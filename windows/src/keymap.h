#pragma once
#include <cstdint>

// Windows 扫描码原样传输；bit8 表示 E0。Pause 单独使用 bit9 标记虚拟键。
inline int WindowsKeyFromScanCode(unsigned scan, bool extended, unsigned virtualKey = 0) {
    if (virtualKey == 0x13) return 0x200 | 0x13;
    if (!scan || scan > 0xff) return -1;
    return static_cast<int>(scan | (extended ? 0x100 : 0));
}
