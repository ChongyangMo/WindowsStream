#include "stream.h"
#include <array>
#include <cstring>

namespace filedrag {
HRESULT RemoteStream::Read(void* data, ULONG size, ULONG* read) {
    if (read) *read = 0;
    if (size && !data) return STG_E_INVALIDPOINTER;
    std::lock_guard lock(mutex_);
    const auto end = offer_->files[index_].size;
    ULONG copied = 0;
    while (copied < size && position_ < end) {
        if (position_ < bufferOffset_ || position_ - bufferOffset_ >= buffer_.size()) {
            const auto length = static_cast<uint32_t>(std::min<uint64_t>(end - position_, kReadSize));
            if (!offer_->peer->Read(offer_->id, index_, position_, length, buffer_)) {
                if (read) *read = copied;
                return STG_E_READFAULT;
            }
            bufferOffset_ = position_;
        }
        const auto offset = static_cast<size_t>(position_ - bufferOffset_);
        const auto count = static_cast<ULONG>(std::min<size_t>(size - copied, buffer_.size() - offset));
        std::memcpy(static_cast<uint8_t*>(data) + copied, buffer_.data() + offset, count);
        position_ += count;
        copied += count;
    }
    if (read) *read = copied;
    return copied == size ? S_OK : S_FALSE;
}

HRESULT RemoteStream::Seek(LARGE_INTEGER offset, DWORD origin, ULARGE_INTEGER* position) {
    std::lock_guard lock(mutex_);
    uint64_t base = 0;
    switch (origin) {
        case STREAM_SEEK_SET: break;
        case STREAM_SEEK_CUR: base = position_; break;
        case STREAM_SEEK_END: base = offer_->files[index_].size; break;
        default: return STG_E_INVALIDFUNCTION;
    }
    if (offset.QuadPart < 0) {
        const auto backwards = uint64_t{0} - static_cast<uint64_t>(offset.QuadPart);
        if (backwards > base) return STG_E_INVALIDFUNCTION;
        position_ = base - backwards;
    } else {
        const auto forwards = static_cast<uint64_t>(offset.QuadPart);
        if (forwards > INT64_MAX - base) return STG_E_INVALIDFUNCTION;
        position_ = base + forwards;
    }
    if (position) position->QuadPart = position_;
    return S_OK;
}

HRESULT RemoteStream::CopyTo(IStream* dest, ULARGE_INTEGER size, ULARGE_INTEGER* read, ULARGE_INTEGER* written) {
    if (read) read->QuadPart = 0;
    if (written) written->QuadPart = 0;
    if (!dest) return STG_E_INVALIDPOINTER;
    std::array<uint8_t, 64 * 1024> block;
    while (size.QuadPart) {
        ULONG got = 0;
        const auto count = static_cast<ULONG>(std::min<uint64_t>(size.QuadPart, block.size()));
        const auto result = Read(block.data(), count, &got);
        if (read) read->QuadPart += got;
        if (FAILED(result)) return result;
        if (got) {
            ULONG put = 0;
            const auto write = dest->Write(block.data(), got, &put);
            if (written) written->QuadPart += put;
            if (FAILED(write)) return write;
            if (put != got) return STG_E_MEDIUMFULL;
        }
        if (result == S_FALSE) return S_FALSE;
        size.QuadPart -= got;
    }
    return S_OK;
}

HRESULT RemoteStream::Stat(STATSTG* info, DWORD flags) {
    if (!info) return STG_E_INVALIDPOINTER;
    *info = {};
    const auto& file = offer_->files[index_];
    if (!(flags & STATFLAG_NONAME)) {
        const auto length = (file.name.size() + 1) * sizeof(wchar_t);
        info->pwcsName = static_cast<wchar_t*>(CoTaskMemAlloc(length));
        if (!info->pwcsName) return STG_E_INSUFFICIENTMEMORY;
        std::memcpy(info->pwcsName, file.name.c_str(), length);
    }
    info->type = STGTY_STREAM;
    info->grfMode = STGM_READ;
    info->cbSize.QuadPart = file.size;
    info->mtime = {static_cast<DWORD>(file.modified), static_cast<DWORD>(file.modified >> 32)};
    return S_OK;
}

HRESULT RemoteStream::Clone(IStream** stream) {
    if (!stream) return E_POINTER;
    std::lock_guard lock(mutex_);
    auto clone = Microsoft::WRL::Make<RemoteStream>(offer_, index_, position_);
    *stream = clone.Detach();
    return *stream ? S_OK : E_OUTOFMEMORY;
}
}
