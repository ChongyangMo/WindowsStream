#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

bool HmacSha256(const std::string& key, const uint8_t* data, size_t size, uint8_t out[32]);
bool SecureEqual(const uint8_t* left, const uint8_t* right, size_t size);
