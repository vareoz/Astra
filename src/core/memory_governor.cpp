#include "core/memory_governor.hpp"

#include "core/platform.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace astra {

MemoryGovernor::MemoryGovernor(uint64_t ceiling_bytes)
    : ceiling_bytes_(ceiling_bytes) {
    register_process(getpid(), "main");
}

void MemoryGovernor::set_ceiling_bytes(uint64_t ceiling_bytes) {
    ceiling_bytes_ = std::max<uint64_t>(ceiling_bytes, 16ULL * 1024 * 1024);
}

void MemoryGovernor::register_process(pid_t pid, std::string role) {
    for (auto& [p, r] : tracked_pids_) {
        if (p == pid) {
            r = std::move(role);
            return;
        }
    }
    tracked_pids_.emplace_back(pid, std::move(role));
}

void MemoryGovernor::unregister_process(pid_t pid) {
    tracked_pids_.erase(
        std::remove_if(tracked_pids_.begin(), tracked_pids_.end(),
                       [pid](const auto& entry) { return entry.first == pid; }),
        tracked_pids_.end());
}

uint64_t MemoryGovernor::read_process_rss_bytes(pid_t pid) {
    // Linux: /proc/<pid>/statm — Darwin: proc_pid_rusage/proc_pidinfo (libproc).
    return platform::query_process_memory(pid).rss_bytes;
}

uint64_t MemoryGovernor::read_process_vsize_bytes(pid_t pid) {
    return platform::query_process_memory(pid).vsize_bytes;
}

MemorySnapshot MemoryGovernor::sample(uint64_t extra_tracked_bytes) const {
    MemorySnapshot snap;
    snap.ceiling_bytes = ceiling_bytes_;
    snap.soft_watermark_bytes = (ceiling_bytes_ * 70) / 100;
    snap.hard_watermark_bytes = (ceiling_bytes_ * 85) / 100;

    uint64_t total = extra_tracked_bytes;
    for (const auto& [pid, role] : tracked_pids_) {
        uint64_t rss = read_process_rss_bytes(pid);
        uint64_t vsize = read_process_vsize_bytes(pid);
        if (rss == 0 && vsize == 0) continue;
        snap.processes.push_back(ProcessMemoryInfo{pid, role, rss, vsize});
        total += rss;
    }
    snap.total_rss_bytes = total;

    if (total >= snap.hard_watermark_bytes) {
        snap.pressure = MemoryPressure::Critical;
    } else if (total >= snap.soft_watermark_bytes) {
        snap.pressure = MemoryPressure::Soft;
    } else {
        snap.pressure = MemoryPressure::Normal;
    }
    return snap;
}

MemoryGovernor::EnforceResult MemoryGovernor::enforce(
    const std::function<void()>& on_flush_caches,
    const std::function<bool()>& on_dehydrate_lru_tab,
    uint64_t extra_tracked_bytes) const {
    EnforceResult res;
    res.before = sample(extra_tracked_bytes);

    if (res.before.pressure == MemoryPressure::Normal) {
        res.after = res.before;
        return res;
    }

    if (on_flush_caches) {
        on_flush_caches();
        res.cache_flushed = true;
    }

    MemorySnapshot cur = sample(extra_tracked_bytes);
    size_t guard = 0;
    while (cur.pressure != MemoryPressure::Normal && guard < 1024) {
        ++guard;
        if (!on_dehydrate_lru_tab || !on_dehydrate_lru_tab()) {
            break;
        }
        ++res.tabs_dehydrated;
        cur = sample(extra_tracked_bytes);
    }
    res.after = cur;
    return res;
}

} // namespace astra
