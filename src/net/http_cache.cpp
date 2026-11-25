#include "net/http_cache.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace astra {

namespace {

int64_t now_epoch_sec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t fnv1a_64(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

HttpCache::HttpCache(std::string cache_dir, size_t max_memory_bytes)
    : cache_dir_(std::move(cache_dir)), max_memory_bytes_(max_memory_bytes) {
    stats_.max_bytes = max_memory_bytes_;
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
}

std::string HttpCache::disk_path_for(const std::string& url) const {
    std::ostringstream oss;
    oss << cache_dir_ << "/" << std::hex << std::setw(16) << std::setfill('0') << fnv1a_64(url) << ".acache";
    return oss.str();
}

CacheControl HttpCache::parse_cache_control(const std::string& cache_control) {
    CacheControl cc;
    std::string low = cache_control;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    cc.no_store = low.find("no-store") != std::string::npos;
    // RFC 9111 §5.2.2: `no-cache` y `must-revalidate` NO prohíben almacenar;
    // obligan a revalidar con el servidor antes de reutilizar la copia.
    cc.must_revalidate = low.find("no-cache") != std::string::npos ||
                         low.find("must-revalidate") != std::string::npos;

    size_t pos = low.find("max-age=");
    if (pos == std::string::npos) {
        cc.max_age_sec = cc.must_revalidate ? 0 : 300;
    } else {
        cc.max_age_sec = std::max<int64_t>(0, std::atoll(low.c_str() + pos + 8));
    }
    if (cc.must_revalidate) cc.max_age_sec = 0;
    return cc;
}

int64_t HttpCache::parse_max_age(const std::string& cache_control) {
    CacheControl cc = parse_cache_control(cache_control);
    return cc.no_store ? 0 : cc.max_age_sec;
}

// Formato de registro en disco (Session 8). La primera línea es un marcador de
// versión: un archivo de una versión anterior simplemente no se reconoce y la
// petición vuelve a la red, que es el comportamiento seguro para una caché.
static constexpr const char* kDiskMarker = "ASTRACACHE2";

void HttpCache::write_to_disk(const CachedResource& res) const {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    std::ofstream f(disk_path_for(res.url), std::ios::binary);
    if (!f) return;
    f << kDiskMarker << "\n"
      << res.url << "\n"
      << res.content_type << "\n"
      << res.etag << "\n"
      << res.last_modified << "\n"
      << res.stored_epoch_sec << " " << res.max_age_sec << " "
      << (res.must_revalidate ? 1 : 0) << " " << res.body.size() << "\n";
    f.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
}

bool HttpCache::load_from_disk(const std::string& url, CachedResource& out) const {
    std::ifstream f(disk_path_for(url), std::ios::binary);
    if (!f) return false;

    std::string marker, stored_url, ctype, etag, last_mod;
    if (!std::getline(f, marker) || marker != kDiskMarker) return false;
    if (!std::getline(f, stored_url) || stored_url != url) return false;
    if (!std::getline(f, ctype)) return false;
    if (!std::getline(f, etag)) return false;
    if (!std::getline(f, last_mod)) return false;

    int64_t epoch = 0, max_age = 0;
    int must_rev = 0;
    size_t body_len = 0;
    if (!(f >> epoch >> max_age >> must_rev >> body_len)) return false;
    if (body_len > 16ULL * 1024 * 1024) return false;
    f.get(); // consume el salto de línea

    std::string body(body_len, '\0');
    if (body_len > 0 && !f.read(body.data(), static_cast<std::streamsize>(body_len))) return false;

    out = CachedResource{};
    out.url = stored_url;
    out.content_type = ctype;
    out.etag = etag;
    out.body = std::move(body);
    out.stored_epoch_sec = epoch;
    out.max_age_sec = max_age;
    out.last_modified = last_mod;
    out.must_revalidate = (must_rev != 0);
    return true;
}

bool HttpCache::lookup(const std::string& url, CachedResource& out) {
    const int64_t now = now_epoch_sec();

    auto is_fresh = [now](const CachedResource& r) {
        return !r.must_revalidate && r.max_age_sec > 0 &&
               (now - r.stored_epoch_sec) <= r.max_age_sec;
    };

    // 1. Caché acotada en RAM. Una entrada caducada se conserva (el cuerpo
    //    sigue sirviendo para revalidar con su ETag), sólo deja de ser un acierto.
    auto it = mem_entries_.find(url);
    if (it != mem_entries_.end()) {
        if (is_fresh(it->second.res)) {
            it->second.last_access_tick = ++tick_;
            out = it->second.res;
            ++stats_.hits;
            return true;
        }
        ++stats_.misses;
        return false;
    }

    // 2. Caché persistente en disco.
    CachedResource disk;
    if (load_from_disk(url, disk)) {
        const bool fresh = is_fresh(disk);
        // Se promueve a RAM aunque esté caducada: así la siguiente petición
        // puede emitir el GET condicional sin volver a tocar el disco.
        if (disk.body.size() <= max_memory_bytes_ / 4) {
            stats_.current_bytes += disk.body.size();
            mem_entries_[url] = MemSlot{disk, ++tick_};
            evict_if_needed();
        }
        if (fresh) {
            out = std::move(disk);
            ++stats_.hits;
            return true;
        }
    }

    ++stats_.misses;
    return false;
}

bool HttpCache::lookup_for_revalidation(const std::string& url, CachedResource& out) {
    const int64_t now = now_epoch_sec();

    auto it = mem_entries_.find(url);
    if (it != mem_entries_.end() && it->second.res.has_validator()) {
        out = it->second.res;
        out.stale = (out.must_revalidate || out.max_age_sec <= 0 ||
                     (now - out.stored_epoch_sec) > out.max_age_sec);
        return true;
    }

    CachedResource disk;
    if (load_from_disk(url, disk) && disk.has_validator()) {
        out = std::move(disk);
        out.stale = (out.must_revalidate || out.max_age_sec <= 0 ||
                     (now - out.stored_epoch_sec) > out.max_age_sec);
        return true;
    }
    return false;
}

bool HttpCache::mark_revalidated(const std::string& url, const CacheControl& cc) {
    CachedResource res;
    auto it = mem_entries_.find(url);
    if (it != mem_entries_.end()) {
        res = it->second.res;
    } else if (!load_from_disk(url, res)) {
        return false;
    }

    res.stored_epoch_sec = now_epoch_sec();
    res.max_age_sec = cc.max_age_sec;
    res.must_revalidate = cc.must_revalidate;
    res.stale = false;

    if (it != mem_entries_.end()) {
        it->second.res = res;
        it->second.last_access_tick = ++tick_;
    } else if (res.body.size() <= max_memory_bytes_ / 4) {
        stats_.current_bytes += res.body.size();
        mem_entries_[url] = MemSlot{res, ++tick_};
        evict_if_needed();
    }
    write_to_disk(res);
    ++stats_.not_modified;
    return true;
}

void HttpCache::store(const CachedResource& entry) {
    // Session 8: también se guarda una respuesta con max-age 0 cuando trae
    // validadores y `must-revalidate`, porque es exactamente el caso en el que
    // la revalidación condicional ahorra transferir el cuerpo otra vez.
    const bool storable = (entry.max_age_sec > 0) ||
                          (entry.must_revalidate && entry.has_validator());
    if (!storable || entry.body.size() > 16ULL * 1024 * 1024) return;

    CachedResource copy = entry;
    copy.stale = false;
    if (copy.stored_epoch_sec == 0) copy.stored_epoch_sec = now_epoch_sec();

    write_to_disk(copy);

    // Store in RAM cache if within per-entry limit
    if (copy.body.size() <= max_memory_bytes_ / 2) {
        auto it = mem_entries_.find(copy.url);
        if (it != mem_entries_.end()) {
            stats_.current_bytes -= it->second.res.body.size();
        }
        stats_.current_bytes += copy.body.size();
        mem_entries_[copy.url] = MemSlot{std::move(copy), ++tick_};
        evict_if_needed();
    }
    ++stats_.stores;
}

void HttpCache::evict_if_needed() {
    while (stats_.current_bytes > max_memory_bytes_ && !mem_entries_.empty()) {
        auto oldest = mem_entries_.begin();
        for (auto it = mem_entries_.begin(); it != mem_entries_.end(); ++it) {
            if (it->second.last_access_tick < oldest->second.last_access_tick) {
                oldest = it;
            }
        }
        stats_.current_bytes -= oldest->second.res.body.size();
        mem_entries_.erase(oldest);
        ++stats_.evictions;
    }
}

void HttpCache::clear() {
    mem_entries_.clear();
    stats_.current_bytes = 0;
}

} // namespace astra
