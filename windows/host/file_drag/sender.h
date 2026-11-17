#pragma once

#include "capture.h"
#include "../../shared/file_drag_source.h"
#include <map>

class HostChannel;
class InputInjector;

namespace filedrag {
class Sender {
public:
    explicit Sender(HostChannel& channel) : channel_(channel) {}
    bool Start();
    bool Handle(uint8_t type, const std::vector<uint8_t>& payload, InputInjector& input);
private:
    struct OfferedFiles {
        std::vector<Source> files;
        bool accepted = false;
    };
    void Probe(uint32_t id, InputInjector& input);
    void Read(const std::vector<uint8_t>& payload);

    HostChannel& channel_;
    Capture capture_;
    bool enabled_ = false;
    std::map<uint32_t, OfferedFiles> offers_;
};
}
