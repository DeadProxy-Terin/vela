#include "platform/ThumbnailCache.hpp"

#include <objbase.h>
#include <shobjidl.h>
#include <algorithm>

namespace vela {

ThumbnailCache::~ThumbnailCache() {
    Stop();
}

void ThumbnailCache::Start(ID3D11Device* device, size_t maxEntries, ReadyCallback onThumbReady) {
    Stop();

    m_device = device;
    m_maxEntries = (std::max)(size_t{64}, maxEntries);
    m_onThumbReady = std::move(onThumbReady);
    m_running = true;

    const unsigned int hw = std::thread::hardware_concurrency();
    const unsigned int threadCount = std::clamp(hw / 2, 2u, 4u);
    m_workers.reserve(threadCount);
    for (unsigned int i = 0; i < threadCount; ++i) {
        m_workers.emplace_back(&ThumbnailCache::WorkerLoop, this);
    }
}

void ThumbnailCache::Stop() {
    {
        std::lock_guard lock(m_queueMutex);
        if (!m_running) return;
        m_running = false;
        while (!m_taskQueue.empty()) m_taskQueue.pop();
        m_inFlight.clear();
    }
    m_cv.notify_all();
    for (auto& t : m_workers) {
        if (t.joinable()) t.join();
    }
    m_workers.clear();

    {
        std::lock_guard lock(m_uploadMutex);
        while (!m_uploadQueue.empty()) m_uploadQueue.pop();
    }

    m_cache.clear();
    m_lruOrder.clear();
    m_failedKeys.clear();
    m_device = nullptr;
}

void ThumbnailCache::SetMaxEntries(size_t maxEntries) {
    m_maxEntries = (std::max)(size_t{64}, maxEntries);
}

void ThumbnailCache::SetGeneration(uint64_t generation) {
    m_activeGeneration.store(generation);
    std::lock_guard lock(m_queueMutex);

    std::queue<Task> fresh;
    while (!m_taskQueue.empty()) {
        Task t = std::move(m_taskQueue.front());
        m_taskQueue.pop();
        if (t.generation == generation) {
            fresh.push(std::move(t));
        } else {
            m_inFlight.erase(t.fullKey);
        }
    }
    m_taskQueue = std::move(fresh);
}

void ThumbnailCache::PumpUploads(size_t maxUploadsPerFrame) {
    if (!m_device) return;

    size_t uploaded = 0;
    while (uploaded < maxUploadsPerFrame) {
        DecodedBitmap bmp;
        {
            std::lock_guard lock(m_uploadMutex);
            if (m_uploadQueue.empty()) break;
            bmp = std::move(m_uploadQueue.front());
            m_uploadQueue.pop();
        }

        if (bmp.width <= 0 || bmp.height <= 0 || bmp.bgraPixels.empty()) {
            continue;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(bmp.width);
        desc.Height = static_cast<UINT>(bmp.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = bmp.bgraPixels.data();
        initData.SysMemPitch = static_cast<UINT>(bmp.width * 4);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (FAILED(m_device->CreateTexture2D(&desc, &initData, &tex))) {
            continue;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
            continue;
        }

        auto existing = m_cache.find(bmp.fullKey);
        if (existing != m_cache.end()) {
            m_lruOrder.erase(existing->second.lruIt);
            m_cache.erase(existing);
        }

        m_lruOrder.push_front(bmp.fullKey);
        m_cache.emplace(bmp.fullKey, CacheNode{std::move(srv), bmp.width, bmp.height, m_lruOrder.begin()});
        ++uploaded;
    }

    // Evict least-recently-used entries before UI frame draws
    while (m_cache.size() > m_maxEntries && !m_lruOrder.empty()) {
        const std::string victim = m_lruOrder.back();
        m_lruOrder.pop_back();
        m_cache.erase(victim);
    }
}

TextureView ThumbnailCache::GetOrRequest(const std::string& thumbKey, const std::wstring& fullPathW,
                                         int sizePx, uint64_t generation) {
    const int bucketSize = std::clamp(sizePx, 24, 256);
    const std::string fullKey = thumbKey + "#" + std::to_string(bucketSize);

    if (auto it = m_cache.find(fullKey); it != m_cache.end()) {
        m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder, it->second.lruIt);
        return TextureView{it->second.srv.Get(), it->second.width, it->second.height};
    }

    std::lock_guard lock(m_queueMutex);
    if (m_failedKeys.find(fullKey) != m_failedKeys.end()) {
        return {};
    }
    if (m_inFlight.find(fullKey) == m_inFlight.end()) {
        m_inFlight.insert(fullKey);
        m_taskQueue.push(Task{fullKey, fullPathW, bucketSize, generation});
        m_cv.notify_one();
    }
    return {};
}

void ThumbnailCache::WorkerLoop() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    while (true) {
        Task task;
        {
            std::unique_lock lock(m_queueMutex);
            m_cv.wait(lock, [this]() { return !m_running || !m_taskQueue.empty(); });
            if (!m_running) break;
            task = std::move(m_taskQueue.front());
            m_taskQueue.pop();
        }

        if (task.generation != m_activeGeneration.load()) {
            std::lock_guard lock(m_queueMutex);
            m_inFlight.erase(task.fullKey);
            continue;
        }

        DecodedBitmap bmp;
        bmp.fullKey = task.fullKey;
        const bool ok = ExtractBitmap(task.fullPathW, task.sizePx, bmp);

        {
            std::lock_guard lock(m_queueMutex);
            m_inFlight.erase(task.fullKey);
            if (!ok) {
                m_failedKeys.insert(task.fullKey);
            }
        }

        if (ok) {
            {
                std::lock_guard lock(m_uploadMutex);
                m_uploadQueue.push(std::move(bmp));
            }
            if (m_onThumbReady) {
                m_onThumbReady();
            }
        }
    }

    ::CoUninitialize();
}

bool ThumbnailCache::ExtractBitmap(const std::wstring& pathW, int sizePx, DecodedBitmap& out) {
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(::SHCreateItemFromParsingName(pathW.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    HBITMAP hBmp = nullptr;
    const SIZE req{sizePx, sizePx};
    HRESULT hr = factory->GetImage(req, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hBmp);
    if (FAILED(hr) || !hBmp) {
        hr = factory->GetImage(req, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hBmp);
    }
    if (FAILED(hr) || !hBmp) {
        return false;
    }

    BITMAP bm{};
    if (::GetObjectW(hBmp, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        ::DeleteObject(hBmp);
        return false;
    }

    const int w = bm.bmWidth;
    const int h = bm.bmHeight;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // Top-down BGRA rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    out.width = w;
    out.height = h;
    out.bgraPixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

    HDC hdc = ::CreateCompatibleDC(nullptr);
    const int scanLines = ::GetDIBits(hdc, hBmp, 0, static_cast<UINT>(h), out.bgraPixels.data(), &bmi, DIB_RGB_COLORS);
    ::DeleteDC(hdc);
    ::DeleteObject(hBmp);

    if (scanLines == 0) {
        out.bgraPixels.clear();
        return false;
    }

    // Fix zeroed alpha on legacy 24-bit bitmaps & un-premultiply premultiplied alpha for halo-free edges
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    bool anyNonZeroAlpha = false;
    bool seemsPremultiplied = true;

    for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t b = out.bgraPixels[i * 4 + 0];
        const uint8_t g = out.bgraPixels[i * 4 + 1];
        const uint8_t r = out.bgraPixels[i * 4 + 2];
        const uint8_t a = out.bgraPixels[i * 4 + 3];
        if (a != 0) anyNonZeroAlpha = true;
        if (r > a || g > a || b > a) seemsPremultiplied = false;
    }

    if (!anyNonZeroAlpha) {
        for (size_t i = 0; i < pixelCount; ++i) {
            out.bgraPixels[i * 4 + 3] = 255;
        }
    } else if (seemsPremultiplied) {
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t a = out.bgraPixels[i * 4 + 3];
            if (a > 0 && a < 255) {
                out.bgraPixels[i * 4 + 0] = static_cast<uint8_t>((std::min)(255, (out.bgraPixels[i * 4 + 0] * 255 + a / 2) / a));
                out.bgraPixels[i * 4 + 1] = static_cast<uint8_t>((std::min)(255, (out.bgraPixels[i * 4 + 1] * 255 + a / 2) / a));
                out.bgraPixels[i * 4 + 2] = static_cast<uint8_t>((std::min)(255, (out.bgraPixels[i * 4 + 2] * 255 + a / 2) / a));
            }
        }
    }

    return true;
}

} // namespace vela