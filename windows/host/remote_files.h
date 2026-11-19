#pragma once
#include "channel.h"

class RemoteFilesHost {
public:
    explicit RemoteFilesHost(HostChannel& channel) : channel_(channel) {}
    bool Start(bool allowed, const std::wstring& ini);
    bool Handle(uint8_t type, const std::vector<uint8_t>& request);
private:
    struct Root { std::wstring name, directory; };
    void LoadRoots(const std::wstring& ini);
    DWORD List(const std::wstring& target, Writer& output) const;
    DWORD Read(const std::wstring& target, bool tail, Writer& output) const;
    const Root* FindRoot(const std::wstring& name) const;
    HostChannel& channel_;
    std::vector<Root> roots_;
};
