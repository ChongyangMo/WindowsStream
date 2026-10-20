#include "clipboard.h"

bool ClipboardBridge::Read(std::string& text) {
    std::lock_guard lock(mutex_);
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == sequence_ || !OpenClipboard(owner_)) return false;
    bool ok = false;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        const size_t capacity = GlobalSize(data) / sizeof(wchar_t);
        if (capacity > 0 && capacity <= 2 * 1024 * 1024) {
            if (const auto* content = static_cast<const wchar_t*>(GlobalLock(data))) {
                size_t length = 0;
                while (length < capacity && content[length]) ++length;
                if (length < capacity) {
                    text = WideToUtf8(std::wstring(content, length));
                    ok = text.size() <= 4 * 1024 * 1024;
                }
                GlobalUnlock(data);
            }
        }
    }
    sequence_ = sequence;
    CloseClipboard();
    return ok;
}

bool ClipboardBridge::Apply(const std::vector<uint8_t>& utf8) {
    if (utf8.size() > 4 * 1024 * 1024) return false;
    std::wstring text = Utf8ToWide(std::string(utf8.begin(), utf8.end()));
    if (!utf8.empty() && text.empty()) return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory) return false;
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return false; }
    memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    std::lock_guard lock(mutex_);
    if (!OpenClipboard(owner_)) { GlobalFree(memory); return false; }
    const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    if (!ok) GlobalFree(memory);
    else sequence_ = GetClipboardSequenceNumber();
    CloseClipboard();
    return ok;
}
