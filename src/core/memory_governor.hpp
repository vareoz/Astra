#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace astra {

enum class MemoryPressure {
    Normal,     // < 70% of hard ceiling
    Soft,       // 70% - 85%: flush caches & compact arenas
    Critical    // >= 85%: dehydrate LRU background tabs immediately
};

struct ProcessMemoryInfo {
    pid_t pid = 0;
    std::string role;
    uint64_t rss_bytes = 0;
    uint64_t vsize_bytes = 0;
};

struct MemorySnapshot {
    uint64_t total_rss_bytes = 0;
    uint64_t ceiling_bytes = 0;
    uint64_t soft_watermark_bytes = 0;
    uint64_t hard_watermark_bytes = 0;
    MemoryPressure pressure = MemoryPressure::Normal;
    std::vector<ProcessMemoryInfo> processes;
};

// Reads real Linux kernel RSS (/proc/<pid>/statm) across Main + Content + RDD
// worker processes and enforces Astra's hard <= 2 GB RAM ceiling.
class MemoryGovernor {
public:
    static constexpr uint64_t kDefaultCeilingBytes = 2ULL * 1024 * 1024 * 1024; // 2.0 GiB

    explicit MemoryGovernor(uint64_t ceiling_bytes = kDefaultCeilingBytes);

    void set_ceiling_bytes(uint64_t ceiling_bytes);
    uint64_t ceiling_bytes() const { return ceiling_bytes_; }

    void register_process(pid_t pid, std::string role);
    void unregister_process(pid_t pid);

    // Reads real OS RSS for a given pid from /proc/<pid>/statm.
    static uint64_t read_process_rss_bytes(pid_t pid);
    static uint64_t read_process_vsize_bytes(pid_t pid);

    MemorySnapshot sample(uint64_t extra_tracked_bytes = 0) const;

    // Evaluates pressure and invokes callbacks until RSS returns safely below
    // the soft watermark (or no more tabs can be dehydrated).
    struct EnforceResult {
        MemorySnapshot before;
        MemorySnapshot after;
        size_t tabs_dehydrated = 0;
        bool cache_flushed = false;
    };

    EnforceResult enforce(
        const std::function<void()>& on_flush_caches,
        const std::function<bool()>& on_dehydrate_lru_tab,
        uint64_t extra_tracked_bytes = 0) const;

private:
    uint64_t ceiling_bytes_;
    std::vector<std::pair<pid_t, std::string>> tracked_pids_;
};

} // namespace astra
