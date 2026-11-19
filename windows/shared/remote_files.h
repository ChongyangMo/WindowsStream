#pragma once
#include "../src/common.h"

namespace remotefiles {
enum class Operation : uint8_t { List = 1, Read, Tail };
constexpr size_t kMaxTarget = 1024;
constexpr DWORD kMaxContent = 4 * 1024 * 1024;
constexpr size_t kRequestHeader = 9;  // u64 请求编号, u8 操作
constexpr size_t kResultHeader = 12;  // u64 请求编号, u32 Win32 错误码

inline Writer Result(uint64_t request, DWORD error) {
    Writer writer;
    writer.u64(request);
    writer.u32(error);
    return writer;
}
}
