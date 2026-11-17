#pragma once

#include "transfer.h"
#include <wrl/implements.h>

namespace filedrag {
class RemoteStream final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::ChainInterfaces<IStream, ISequentialStream>, Microsoft::WRL::FtmBase> {
public:
    RemoteStream(std::shared_ptr<Offer> offer, uint16_t index, uint64_t position = 0)
        : offer_(std::move(offer)), index_(index), position_(position) {}
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override;
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER offset, DWORD origin, ULARGE_INTEGER* position) override;
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* dest, ULARGE_INTEGER size, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override;
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* info, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Clone(IStream** stream) override;

private:
    std::shared_ptr<Offer> offer_;
    uint16_t index_;
    std::mutex mutex_;
    uint64_t position_;
    uint64_t bufferOffset_ = 0;
    std::vector<uint8_t> buffer_;
};
}
