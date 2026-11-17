#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vela {

struct TextureView {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
};

class ThumbnailCache {
public:
    using ReadyCallback = std::function<void()>;

    ThumbnailCache() = default;
    ~ThumbnailCache();

    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    void Start(ID3D11Device* device, size_t maxEntries, ReadyCallback onThumbReady);
    void Stop();

    void SetMaxEntries(size_t maxEntries);
    void SetGeneration(uint64_t generation);

    // Must be called on the UI thread BEFORE building ImGui draw lists for the frame
    void PumpUploads(size_t maxUploadsPerFrame = 16);

    // Non-blocking lookup; queues background extraction on cache miss
    TextureView GetOrRequest(const std::string& thumbKey, const std::wstring& fullPathW, int sizePx, uint64_t generation);

private:
    struct Task {
        std::string fullKey;
        std::wstring fullPathW;
        int sizePx = 64;
        uint64_t generation = 0;
    };

    struct DecodedBitmap {
        std::string fullKey;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> bgraPixels;
    };

    struct CacheNode {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0;
        int height = 0;
        std::list<std::string>::iterator lruIt;
    };

    void WorkerLoop();
    bool ExtractBitmap(const std::wstring& pathW, int sizePx, DecodedBitmap& out);

    ID3D11Device* m_device = nullptr;
    ReadyCallback m_onThumbReady;

    std::vector<std::thread> m_workers;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<Task> m_taskQueue;
    bool m_running = false;

    std::mutex m_uploadMutex;
    std::queue<DecodedBitmap> m_uploadQueue;

    std::unordered_set<std::string> m_inFlight;
    std::unordered_set<std::string> m_failedKeys;

    std::list<std::string> m_lruOrder;
    std::unordered_map<std::string, CacheNode> m_cache;
    size_t m_maxEntries = 512;

    std::atomic<uint64_t> m_activeGeneration{0};
};

} // namespace vela