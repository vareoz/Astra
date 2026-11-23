#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace astra {

// Firefox nsPresArena-inspired per-tab mmap Slab Arena Allocator.
// - Zero per-node malloc/free overhead during HTML/CSS/Layout construction.
// - Deterministic poison-on-reset (0xDEADBEEF) to catch use-after-free bugs.
// - Instant OS page reclamation via madvise(MADV_DONTNEED) / munmap on tab
//   dehydration or teardown so RSS drops immediately back to the kernel.
class SlabArena {
public:
    static constexpr size_t kDefaultSlabSize = 256 * 1024; // 256 KiB OS slabs
    static constexpr uint32_t kPoisonPattern = 0xDEADBEEFu;

    explicit SlabArena(size_t slab_size = kDefaultSlabSize);
    ~SlabArena();

    SlabArena(const SlabArena&) = delete;
    SlabArena& operator=(const SlabArena&) = delete;
    SlabArena(SlabArena&& other) noexcept;
    SlabArena& operator=(SlabArena&& other) noexcept;

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            destructors_.push_back({obj, [](void* p) { static_cast<T*>(p)->~T(); }});
        }
        return obj;
    }

    // Intern a string directly inside the arena (zero heap allocation).
    std::string_view intern(std::string_view s);

    // Destroys non-trivial objects, poisons freed memory, and returns unused
    // pages to the OS kernel via madvise(MADV_DONTNEED) or full munmap.
    void reset(bool release_to_os = true);

    size_t bytes_used() const { return total_used_; }
    size_t bytes_reserved() const { return total_reserved_; }
    size_t slab_count() const { return slabs_.size(); }

private:
    struct Slab {
        uint8_t* base = nullptr;
        size_t capacity = 0;
        size_t offset = 0;
    };

    struct DestructorEntry {
        void* ptr;
        void (*fn)(void*);
    };

    void allocate_slab(size_t min_bytes);
    void release_all_slabs();

    size_t slab_size_;
    std::vector<Slab> slabs_;
    std::vector<DestructorEntry> destructors_;
    size_t total_used_ = 0;
    size_t total_reserved_ = 0;
};

} // namespace astra
