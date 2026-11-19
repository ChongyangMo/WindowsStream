#pragma once
#include "../../shared/file_drag_source.h"

namespace remotefiles {
std::wstring PipeName(const std::wstring& profile);
filedrag::FileHandle CreateServerPipe(const std::wstring& name);
filedrag::FileHandle OpenServerPipe(const std::wstring& name, DWORD& error);
bool AcceptClient(HANDLE pipe, HANDLE stop);
bool Transfer(HANDLE pipe, HANDLE stop, bool write, void* buffer, DWORD size, DWORD& transferred, DWORD timeout);
}
