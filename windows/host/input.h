#pragma once
#include "../src/common.h"
#include <set>

class InputInjector {
public:
    ~InputInjector();
    bool Handle(uint8_t type, const std::vector<uint8_t>& payload);
    void ReleaseAll();
private:
    bool Submit(INPUT input);
    bool Move(uint16_t x, uint16_t y);
    bool Key(uint16_t code, bool down);
    bool Button(uint8_t button, bool down);
    std::set<uint16_t> keys_;
    std::set<uint8_t> buttons_;
    uint64_t lastFailure_ = 0;
};
