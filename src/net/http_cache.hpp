#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace astra {

struct CachedResource {
    std::string url;
    std::string content_type;
    std::string etag;
    std::string body;
    int64_t stored_epoch_sec = 0;
    int64_t max_age_sec = 300;
};

struct CacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t stores = 0;
    uint64_t evictions = 0;
    size_t current_bytes = 0;
    size_t max_bytes = 0;
};

// Firefox Necko Cache2-inspired bounded RAM + Disk HTTP resource cache.
// Enforces a strict byte ceiling (default 32 MiB) with LRU eviction so cached
// subresources never push Astra toward its 2 GB RAM budget.
class HttpCache {
public:
    explicit HttpCache(std::string cache_dir = "/tmp/astra_http_cache",
                       size_t max_memory_bytes = 32ULL * 1024 * 1024);

    bool lookup(const std::string& url, CachedResource& out);
    void store(const CachedResource& entry);
    void clear();

    const CacheStats& stats() const { return stats_; }
    static int64_t parse_max_age(const std::string& cache_control);

private:
    std::string disk_path_for(const std::string& url) const;
    void evict_if_needed();

    struct MemSlot {
        CachedResource res;
        uint64_t last_access_tick = 0;
    };

    std::string cache_dir_;
    size_t max_memory_bytes_;
    uint64_t tick_ = 0;
    std::unordered_map<std::string, MemSlot> mem_entries_;
    CacheStats stats_;
};

} // namespace astra
