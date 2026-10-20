#pragma once
#include "../../shared/proxy/transport.h"

namespace proxy {
class PacServer {
public:
    ~PacServer() { Stop(); }
    bool Start(const std::string& script);
    void Stop();
    const std::wstring& Url() const { return url_; }
private:
    Tasks tasks_;
    std::wstring url_;
    bool winsock_ = false;
};
}
