#pragma once
#include "../src/common.h"

// 缺少采样的阶段使用明确的未知值，不用零耗时替代。
constexpr uint32_t kUnknownMetric = UINT32_MAX;
struct HostTelemetry {
    uint16_t sourceWidth = 0, sourceHeight = 0, refreshHz = 0;
    uint16_t width = 0, height = 0, fpsCap = 0;
    uint8_t pipeline = 0;  // 1 GPU 硬编，2 GPU 软编，3 GDI 软编
    uint32_t sampleMillis = 0;
    uint32_t captureMilliFps = 0, submitMilliFps = 0, outputMilliFps = 0, kbps = 0;
    uint32_t captureUs = kUnknownMetric, submitUs = kUnknownMetric;
    uint32_t outputAgeUs = kUnknownMetric, sendUs = kUnknownMetric;
    uint32_t skipped = 0, pending = 0;
};
Writer EncodeTelemetry(const HostTelemetry& value);
bool DecodeTelemetry(const std::vector<uint8_t>& bytes, HostTelemetry& value);
