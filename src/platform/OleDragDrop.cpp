#include "platform/OleDragDrop.hpp"
#include "core/StringUtil.hpp"

#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <utility>

namespace vela {
namespace {

constexpr GUID kIidUnknown       = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidEnumFormatEtc = {0x00000103, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidDataObject    = {0x0000010E, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidDropSource    = {0x00000121, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidDropTarget    = {0x00000122, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

UINT GetDropEffectFormat() {
    static const UINT fmt = ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return fmt;
}

bool SameIid(REFIID a, const GUID& b) {
    return ::IsEqualGUID(a, b) != FALSE;
}

HGLOBAL AllocGlobalCopy(const void* bytes, size_t sizeBytes) {
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeBytes);
    if (!mem) return nullptr;
    if (void* ptr = ::GlobalLock(mem)) {
        std::memcpy(ptr, bytes, sizeBytes);
        ::GlobalUnlock(mem);
    }
    return mem;
}

HGLOBAL CloneGlobalMem(HGLOBAL src) {
    if (!src) return nullptr;
    const SIZE_T sz = ::GlobalSize(src);
    if (sz == 0) return nullptr;
    const void* srcPtr = ::GlobalLock(src);
    if (!srcPtr) return nullptr;
    HGLOBAL copy = AllocGlobalCopy(srcPtr, static_cast<size_t>(sz));
    ::GlobalUnlock(src);
    return copy;
}

HGLOBAL BuildDropFilesMem(const std::vector<std::wstring>& pathsW) {
    size_t wcharCount = 1; // Trailing double-null terminator
    for (const auto& p : pathsW) {
        wcharCount += p.size() + 1;
    }

    const size_t totalBytes = sizeof(DROPFILES) + wcharCount * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!mem) return nullptr;

    auto* raw = static_cast<uint8_t*>(::GlobalLock(mem));
    if (!raw) {
        ::GlobalFree(mem);
        return nullptr;
    }

    DROPFILES df{};
    df.pFiles = sizeof(DROPFILES);
    df.fWide = TRUE;
    std::memcpy(raw, &df, sizeof(DROPFILES));

    auto* dst = reinterpret_cast<wchar_t*>(raw + sizeof(DROPFILES));
    for (const auto& p : pathsW) {
        std::memcpy(dst, p.data(), p.size() * sizeof(wchar_t));
        dst += p.size();
        *dst++ = L'\0';
    }
    *dst = L'\0';

    ::GlobalUnlock(mem);
    return mem;
}

std::vector<std::wstring> ReadDropFilesFromDataObject(IDataObject* dataObj) {
    std::vector<std::wstring> out;
    if (!dataObj) return out;

    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    if (FAILED(dataObj->GetData(&fmt, &med))) {
        return out;
    }

    if (med.tymed == TYMED_HGLOBAL && med.hGlobal) {
        HDROP hDrop = reinterpret_cast<HDROP>(med.hGlobal);
        const UINT count = ::DragQueryFileW(hDrop, 0xFFFFFFFFu, nullptr, 0);
        out.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = ::DragQueryFileW(hDrop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring path(len, L'\0');
            if (::DragQueryFileW(hDrop, i, path.data(), len + 1) > 0) {
                out.push_back(str::NormalizeWindowsPath(path));
            }
        }
    }

    ::ReleaseStgMedium(&med);
    return out;
}

class FormatEnumerator final : public IEnumFORMATETC {
public:
    FormatEnumerator(std::vector<FORMATETC> formats, size_t index = 0)
        : m_formats(std::move(formats)), m_index(index) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (SameIid(riid, kIidUnknown) || SameIid(riid, kIidEnumFormatEtc)) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override {
        if (!rgelt) return E_INVALIDARG;
        if (celt > 1 && !pceltFetched) return E_INVALIDARG;

        ULONG copied = 0;
        while (copied < celt && m_index < m_formats.size()) {
            rgelt[copied++] = m_formats[m_index++];
        }
        if (pceltFetched) *pceltFetched = copied;
        return (copied == celt) ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override {
        const size_t remaining = (m_index < m_formats.size()) ? (m_formats.size() - m_index) : 0;
        if (celt > remaining) {
            m_index = m_formats.size();
            return S_FALSE;
        }
        m_index += celt;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        m_index = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** ppenum) override {
        if (!ppenum) return E_POINTER;
        *ppenum = new FormatEnumerator(m_formats, m_index);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
    std::vector<FORMATETC> m_formats;
    size_t m_index = 0;
};

class FileTransferDataObject final : public IDataObject {
public:
    FileTransferDataObject(const std::vector<std::wstring>& pathsW, DWORD preferredEffect) {
        if (HGLOBAL hDropMem = BuildDropFilesMem(pathsW)) {
            Entry e{};
            e.fmt = FORMATETC{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            e.mem = hDropMem;
            m_entries.push_back(e);
        }
        if (preferredEffect != 0) {
            if (HGLOBAL effMem = AllocGlobalCopy(&preferredEffect, sizeof(DWORD))) {
                Entry e{};
                e.fmt = FORMATETC{
                    static_cast<CLIPFORMAT>(GetDropEffectFormat()),
                    nullptr,
                    DVASPECT_CONTENT,
                    -1,
                    TYMED_HGLOBAL
                };
                e.mem = effMem;
                m_entries.push_back(e);
            }
        }
    }

    ~FileTransferDataObject() {
        for (auto& e : m_entries) {
            if (e.mem) ::GlobalFree(e.mem);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (SameIid(riid, kIidUnknown) || SameIid(riid, kIidDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* pFmt, STGMEDIUM* pMed) override {
        if (!pFmt || !pMed) return E_INVALIDARG;
        std::memset(pMed, 0, sizeof(STGMEDIUM));

        const Entry* match = FindEntry(*pFmt);
        if (!match) return DV_E_FORMATETC;

        HGLOBAL clone = CloneGlobalMem(match->mem);
        if (!clone) return E_OUTOFMEMORY;

        pMed->tymed = TYMED_HGLOBAL;
        pMed->hGlobal = clone;
        pMed->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* pFmt) override {
        if (!pFmt) return E_INVALIDARG;
        return FindEntry(*pFmt) ? S_OK : DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pOut) override {
        if (pOut) pOut->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        if (dwDirection != DATADIR_GET) {
            *ppEnum = nullptr;
            return E_NOTIMPL;
        }
        std::vector<FORMATETC> list;
        list.reserve(m_entries.size());
        for (const auto& e : m_entries) {
            list.push_back(e.fmt);
        }
        *ppEnum = new FormatEnumerator(std::move(list));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    struct Entry {
        FORMATETC fmt{};
        HGLOBAL mem = nullptr;
    };

    const Entry* FindEntry(const FORMATETC& q) const {
        for (const auto& e : m_entries) {
            if (e.fmt.cfFormat == q.cfFormat &&
                e.fmt.dwAspect == q.dwAspect &&
                (q.tymed & e.fmt.tymed) != 0) {
                return &e;
            }
        }
        return nullptr;
    }

    std::atomic<ULONG> m_refCount{1};
    std::vector<Entry> m_entries;
};

class BasicDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (SameIid(riid, kIidUnknown) || SameIid(riid, kIidDropSource)) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if ((grfKeyState & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> m_refCount{1};
};

class WindowDropTarget final : public IDropTarget {
public:
    WindowDropTarget(HWND hwnd, DragTargetCallbacks callbacks)
        : m_hwnd(hwnd), m_callbacks(std::move(callbacks)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (SameIid(riid, kIidUnknown) || SameIid(riid, kIidDropTarget)) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        m_dragPaths = ReadDropFilesFromDataObject(pDataObj);
        DWORD effect = DROPEFFECT_NONE;
        if (!m_dragPaths.empty() && m_callbacks.onDragEnter) {
            effect = m_callbacks.onDragEnter(ToClientPt(pt), grfKeyState, pdwEffect ? *pdwEffect : 0, m_dragPaths);
        }
        if (pdwEffect) *pdwEffect = effect;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        DWORD effect = DROPEFFECT_NONE;
        if (!m_dragPaths.empty() && m_callbacks.onDragOver) {
            effect = m_callbacks.onDragOver(ToClientPt(pt), grfKeyState, pdwEffect ? *pdwEffect : 0);
        }
        if (pdwEffect) *pdwEffect = effect;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        m_dragPaths.clear();
        if (m_callbacks.onDragLeave) {
            m_callbacks.onDragLeave();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        std::vector<std::wstring> dropped = !m_dragPaths.empty()
            ? std::move(m_dragPaths)
            : ReadDropFilesFromDataObject(pDataObj);
        m_dragPaths.clear();

        DWORD effect = DROPEFFECT_NONE;
        if (!dropped.empty() && m_callbacks.onDrop) {
            effect = m_callbacks.onDrop(ToClientPt(pt), grfKeyState, pdwEffect ? *pdwEffect : 0, std::move(dropped));
        }
        if (pdwEffect) *pdwEffect = effect;
        return S_OK;
    }

private:
    POINT ToClientPt(POINTL pt) const {
        POINT c{pt.x, pt.y};
        if (m_hwnd) ::ScreenToClient(m_hwnd, &c);
        return c;
    }

    std::atomic<ULONG> m_refCount{1};
    HWND m_hwnd = nullptr;
    DragTargetCallbacks m_callbacks;
    std::vector<std::wstring> m_dragPaths;
};

} // namespace

struct OleDragDropHost::Impl {
    HWND hwnd = nullptr;
    Microsoft::WRL::ComPtr<WindowDropTarget> dropTarget;
};

OleDragDropHost::OleDragDropHost() = default;

OleDragDropHost::~OleDragDropHost() {
    Detach();
}

bool OleDragDropHost::Attach(HWND hwnd, DragTargetCallbacks callbacks) {
    Detach();
    if (!hwnd) return false;

    auto impl = std::make_unique<Impl>();
    impl->hwnd = hwnd;
    impl->dropTarget.Attach(new WindowDropTarget(hwnd, std::move(callbacks)));

    if (FAILED(::RegisterDragDrop(hwnd, impl->dropTarget.Get()))) {
        return false;
    }
    m_impl = std::move(impl);
    return true;
}

void OleDragDropHost::Detach() {
    if (!m_impl) return;
    if (m_impl->hwnd) {
        ::RevokeDragDrop(m_impl->hwnd);
    }
    m_impl.reset();
}

DWORD OleDragDropHost::StartFileDrag(const std::vector<std::wstring>& pathsW, DWORD allowedEffects) {
    if (pathsW.empty()) return DROPEFFECT_NONE;

    Microsoft::WRL::ComPtr<IDataObject> dataObj;
    dataObj.Attach(new FileTransferDataObject(pathsW, 0));

    Microsoft::WRL::ComPtr<IDropSource> dropSource;
    dropSource.Attach(new BasicDropSource());

    DWORD performedEffect = DROPEFFECT_NONE;
    const HRESULT hr = ::DoDragDrop(dataObj.Get(), dropSource.Get(), allowedEffects, &performedEffect);
    return (hr == DRAGDROP_S_DROP) ? performedEffect : DROPEFFECT_NONE;
}

bool OleDragDropHost::SetClipboardFiles(const std::vector<std::wstring>& pathsW, bool isCut) {
    if (pathsW.empty()) return false;

    const DWORD eff = isCut ? DROPEFFECT_MOVE : (DROPEFFECT_COPY | DROPEFFECT_LINK);
    Microsoft::WRL::ComPtr<IDataObject> dataObj;
    dataObj.Attach(new FileTransferDataObject(pathsW, eff));

    return SUCCEEDED(::OleSetClipboard(dataObj.Get()));
}

bool OleDragDropHost::HasClipboardFiles() {
    return ::IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

bool OleDragDropHost::ReadClipboardFiles(std::vector<std::wstring>& outPathsW, bool& outIsCut) {
    outPathsW.clear();
    outIsCut = false;

    Microsoft::WRL::ComPtr<IDataObject> dataObj;
    if (FAILED(::OleGetClipboard(dataObj.GetAddressOf())) || !dataObj) {
        return false;
    }

    outPathsW = ReadDropFilesFromDataObject(dataObj.Get());
    if (outPathsW.empty()) return false;

    FORMATETC fmtEff{
        static_cast<CLIPFORMAT>(GetDropEffectFormat()),
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL
    };
    STGMEDIUM med{};
    if (SUCCEEDED(dataObj->GetData(&fmtEff, &med)) && med.tymed == TYMED_HGLOBAL && med.hGlobal) {
        if (::GlobalSize(med.hGlobal) >= sizeof(DWORD)) {
            if (const auto* eff = static_cast<const DWORD*>(::GlobalLock(med.hGlobal))) {
                outIsCut = ((*eff & DROPEFFECT_MOVE) != 0) && ((*eff & DROPEFFECT_COPY) == 0);
                ::GlobalUnlock(med.hGlobal);
            }
        }
        ::ReleaseStgMedium(&med);
    }

    return true;
}

void OleDragDropHost::FlushClipboardOnExit() {
    ::OleFlushClipboard();
}

} // namespace vela