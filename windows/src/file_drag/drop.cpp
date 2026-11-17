#include "drop.h"
#include "stream.h"
#include <shlobj.h>
#include <ShlDisp.h>
#include <cstring>

namespace filedrag {
namespace {
struct ShellFormats {
    const CLIPFORMAT descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const CLIPFORMAT contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    const CLIPFORMAT preferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    const CLIPFORMAT performed = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));
    const CLIPFORMAT logical = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_LOGICALPERFORMEDDROPEFFECT));
    const CLIPFORMAT succeeded = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED));
};

class DataObject final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IDataObject, IDataObjectAsyncCapability, Microsoft::WRL::FtmBase> {
public:
    explicit DataObject(std::shared_ptr<Offer> offer) : offer_(std::move(offer)) {}

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        const auto possible = QueryGetData(format);
        if (FAILED(possible)) return possible;
        if (format->cfFormat == formats_.contents) {
            if (format->lindex == -1 && offer_->files.size() != 1) return DV_E_LINDEX;
            const auto index = static_cast<uint16_t>(std::max(0L, format->lindex));
            auto stream = Microsoft::WRL::Make<RemoteStream>(offer_, index);
            if (!stream) return E_OUTOFMEMORY;
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream.Detach();
            return S_OK;
        }
        const bool descriptor = format->cfFormat == formats_.descriptor;
        const auto size = descriptor
            ? offsetof(FILEGROUPDESCRIPTORW, fgd) + offer_->files.size() * sizeof(FILEDESCRIPTORW)
            : sizeof(DWORD);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (!memory) return E_OUTOFMEMORY;
        auto* data = GlobalLock(memory);
        if (!data) {
            GlobalFree(memory);
            return E_OUTOFMEMORY;
        }
        if (descriptor) Describe(data);
        else *static_cast<DWORD*>(data) = DROPEFFECT_COPY;
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->cfFormat == formats_.contents) {
            if (format->lindex < -1 || format->lindex >= static_cast<LONG>(offer_->files.size())) return DV_E_LINDEX;
            return (format->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
        }
        if (format->cfFormat != formats_.descriptor && format->cfFormat != formats_.preferred) return DV_E_FORMATETC;
        if (format->lindex != -1) return DV_E_LINDEX;
        return (format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium) return E_POINTER;
        if (format->cfFormat != formats_.performed && format->cfFormat != formats_.logical &&
            format->cfFormat != formats_.succeeded) return DV_E_FORMATETC;
        if (!(format->tymed & TYMED_HGLOBAL) || medium->tymed != TYMED_HGLOBAL) return DV_E_TYMED;
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) override {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        const FORMATETC available[] = {
            {formats_.descriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {formats_.contents, nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM},
            {formats_.preferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        return SHCreateStdEnumFmtEtc(ARRAYSIZE(available), available, formats);
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override { async_ = enabled; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override { *enabled = async_; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { inOperation_ = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override { *active = inOperation_; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD) override {
        inOperation_ = FALSE;
        if (FAILED(result) && result != E_ABORT && result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            LogLine("本地接收拖拽文件失败：0x%08X", static_cast<unsigned>(result));
        }
        return S_OK;
    }

private:
    void Describe(void* data) const {
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(data);
        group->cItems = static_cast<UINT>(offer_->files.size());
        auto* descriptions = reinterpret_cast<FILEDESCRIPTORW*>(
            static_cast<uint8_t*>(data) + offsetof(FILEGROUPDESCRIPTORW, fgd));
        for (const auto& file : offer_->files) {
            auto& descriptor = *descriptions++;
            descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI | static_cast<DWORD>(FD_UNICODE);
            descriptor.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            descriptor.nFileSizeHigh = static_cast<DWORD>(file.size >> 32);
            descriptor.nFileSizeLow = static_cast<DWORD>(file.size);
            descriptor.ftLastWriteTime = {static_cast<DWORD>(file.modified), static_cast<DWORD>(file.modified >> 32)};
            std::memcpy(descriptor.cFileName, file.name.c_str(), (file.name.size() + 1) * sizeof(wchar_t));
        }
    }

    ShellFormats formats_;
    std::shared_ptr<Offer> offer_;
    std::atomic<BOOL> async_{TRUE};
    std::atomic<BOOL> inOperation_{FALSE};
};

class DropSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropSource> {
public:
    explicit DropSource(HWND viewer) : root_(GetAncestor(viewer, GA_ROOT)) {}

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD buttons) override {
        if (escape || !IsWindow(root_) || (buttons & MK_RBUTTON)) return DRAGDROP_S_CANCEL;
        if (buttons & MK_LBUTTON) return S_OK;
        POINT point{};
        if (!GetCursorPos(&point) || GetAncestor(WindowFromPoint(point), GA_ROOT) == root_) return DRAGDROP_S_CANCEL;
        return DRAGDROP_S_DROP;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    HWND root_;
};
}

void DropFiles(HWND viewer, const std::shared_ptr<Offer>& offer) {
    auto data = Microsoft::WRL::Make<DataObject>(offer);
    auto source = Microsoft::WRL::Make<DropSource>(viewer);
    if (!data || !source) return;
    DWORD effect = DROPEFFECT_NONE;
    const auto result = DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY, &effect);
    if (FAILED(result)) LogLine("无法开始本地文件拖拽：0x%08X", static_cast<unsigned>(result));
}
}
