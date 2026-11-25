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
    // Session 8 — validadores y directivas de frescura (RFC 9111).
    std::string last_modified;      // valor literal de la cabecera Last-Modified
    bool must_revalidate = false;   // Cache-Control: no-cache / must-revalidate
    bool stale = false;             // relleno por lookup_for_revalidation()

    bool has_validator() const { return !etag.empty() || !last_modified.empty(); }
};

// Directivas de Cache-Control que interesan al almacenamiento.
struct CacheControl {
    int64_t max_age_sec = 300;
    bool no_store = false;
    bool must_revalidate = false;

    // Un recurso se guarda si no está prohibido y, o bien sigue fresco un
    // tiempo, o bien conserva validadores para revalidarlo después.
    bool storable() const { return !no_store && (max_age_sec > 0 || must_revalidate); }
};

struct CacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t stores = 0;
    uint64_t evictions = 0;
    uint64_t revalidations = 0;   // GET condicionales emitidos
    uint64_t not_modified = 0;    // respuestas 304 servidas desde caché
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

    // Devuelve true sólo si la entrada sigue fresca y utilizable sin consultar
    // al servidor. Una entrada caducada o marcada `must-revalidate` NO se
    // destruye: se conserva para poder revalidarla con su validador.
    bool lookup(const std::string& url, CachedResource& out);

    // Session 8: recupera una entrada (fresca o caducada) que conserve algún
    // validador, para emitir un GET condicional
    // (If-None-Match / If-Modified-Since).
    bool lookup_for_revalidation(const std::string& url, CachedResource& out);

    // Session 8: tras un 304 Not Modified, renueva la frescura de la entrada
    // conservando el cuerpo almacenado. Devuelve false si ya no existe.
    bool mark_revalidated(const std::string& url, const CacheControl& cc);

    void note_revalidation_sent() { ++stats_.revalidations; }

    void store(const CachedResource& entry);
    void clear();

    const CacheStats& stats() const { return stats_; }
    static int64_t parse_max_age(const std::string& cache_control);
    static CacheControl parse_cache_control(const std::string& cache_control);

private:
    std::string disk_path_for(const std::string& url) const;
    void evict_if_needed();
    bool load_from_disk(const std::string& url, CachedResource& out) const;
    void write_to_disk(const CachedResource& res) const;

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
