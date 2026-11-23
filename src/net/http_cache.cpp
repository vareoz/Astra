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

int64_t HttpCache::parse_max_age(const std::string& cache_control) {
    std::string low = cache_control;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low.find("no-store") != std::string::npos || low.find("no-cache") != std::string::npos) {
        return 0;
    }
    size_t pos = low.find("max-age=");
    if (pos == std::string::npos) return 300;
    return std::max<int64_t>(0, std::atoll(low.c_str() + pos + 8));
}

bool HttpCache::lookup(const std::string& url, CachedResource& out) {
    int64_t now = now_epoch_sec();

    // 1. Check bounded RAM cache
    auto it = mem_entries_.find(url);
    if (it != mem_entries_.end()) {
        const auto& res = it->second.res;
        if (res.max_age_sec > 0 && (now - res.stored_epoch_sec) <= res.max_age_sec) {
            it->second.last_access_tick = ++tick_;
            out = res;
            ++stats_.hits;
            return true;
        }
        stats_.current_bytes -= it->second.res.body.size();
        mem_entries_.erase(it);
    }

    // 2. Check persistent disk cache
    std::ifstream f(disk_path_for(url), std::ios::binary);
    if (f) {
        std::string stored_url, ctype, etag;
        int64_t epoch = 0, max_age = 0;
        size_t body_len = 0;
        if (std::getline(f, stored_url) && std::getline(f, ctype) && std::getline(f, etag)) {
            if ((f >> epoch >> max_age >> body_len) && stored_url == url) {
                f.get(); // consume newline
                if (max_age > 0 && (now - epoch) <= max_age && body_len <= 16ULL * 1024 * 1024) {
                    std::string body(body_len, '\0');
                    if (f.read(body.data(), static_cast<std::streamsize>(body_len))) {
                        out = CachedResource{stored_url, ctype, etag, std::move(body), epoch, max_age};
                        // Promote to bounded RAM cache
                        if (out.body.size() <= max_memory_bytes_ / 4) {
                            mem_entries_[url] = MemSlot{out, ++tick_};
                            stats_.current_bytes += out.body.size();
                            evict_if_needed();
                        }
                        ++stats_.hits;
                        return true;
                    }
                }
            }
        }
    }

    ++stats_.misses;
    return false;
}

void HttpCache::store(const CachedResource& entry) {
    if (entry.max_age_sec <= 0 || entry.body.size() > 16ULL * 1024 * 1024) return;

    CachedResource copy = entry;
    if (copy.stored_epoch_sec == 0) copy.stored_epoch_sec = now_epoch_sec();

    // Write to disk cache
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    std::ofstream f(disk_path_for(copy.url), std::ios::binary);
    if (f) {
        f << copy.url << "\n"
          << copy.content_type << "\n"
          << copy.etag << "\n"
          << copy.stored_epoch_sec << " " << copy.max_age_sec << " " << copy.body.size() << "\n";
        f.write(copy.body.data(), static_cast<std::streamsize>(copy.body.size()));
    }

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
