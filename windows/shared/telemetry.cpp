#include "telemetry.h"

Writer EncodeTelemetry(const HostTelemetry& t) {
    Writer w;
    w.u32(1);
    for (auto v : {t.sourceWidth, t.sourceHeight, t.refreshHz, t.width, t.height, t.fpsCap}) w.u16(v);
    w.u8(t.pipeline); w.u8(0); w.u16(0);
    for (auto v : {t.sampleMillis, t.captureMilliFps, t.submitMilliFps, t.outputMilliFps, t.kbps,
                   t.captureUs, t.submitUs, t.outputAgeUs, t.sendUs, t.skipped, t.pending}) w.u32(v);
    return w;
}

bool DecodeTelemetry(const std::vector<uint8_t>& bytes, HostTelemetry& t) {
    if (bytes.size() != 64 || ReadU32(bytes.data()) != 1) return false;
    const auto* p = bytes.data() + 4;
    uint16_t* sizes[] = {&t.sourceWidth, &t.sourceHeight, &t.refreshHz, &t.width, &t.height, &t.fpsCap};
    for (auto* size : sizes) { *size = ReadU16(p); p += 2; }
    t.pipeline = *p; p += 4;
    uint32_t* values[] = {&t.sampleMillis, &t.captureMilliFps, &t.submitMilliFps, &t.outputMilliFps, &t.kbps,
                         &t.captureUs, &t.submitUs, &t.outputAgeUs, &t.sendUs, &t.skipped, &t.pending};
    for (auto* value : values) { *value = ReadU32(p); p += 4; }
    return t.width > 0 && t.height > 0 && t.fpsCap <= 240 && t.pipeline >= 1 && t.pipeline <= 3;
}
