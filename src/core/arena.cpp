#include "core/arena.hpp"

#include "core/platform.hpp"

#include <algorithm>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace astra {

namespace {
size_t page_align(size_t n) {
    static const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return (n + page_size - 1) & ~(page_size - 1);
}

void poison_buffer(uint8_t* ptr, size_t len) {
    uint32_t* words = reinterpret_cast<uint32_t*>(ptr);
    size_t word_count = len / sizeof(uint32_t);
    for (size_t i = 0; i < word_count; ++i) {
        words[i] = SlabArena::kPoisonPattern;
    }
}
} // namespace

SlabArena::SlabArena(size_t slab_size)
    : slab_size_(page_align(std::max<size_t>(slab_size, 4096))) {}

SlabArena::~SlabArena() {
    reset(true);
    release_all_slabs();
}

SlabArena::SlabArena(SlabArena&& other) noexcept
    : slab_size_(other.slab_size_),
      slabs_(std::move(other.slabs_)),
      destructors_(std::move(other.destructors_)),
      total_used_(other.total_used_),
      total_reserved_(other.total_reserved_) {
    other.total_used_ = 0;
    other.total_reserved_ = 0;
}

SlabArena& SlabArena::operator=(SlabArena&& other) noexcept {
    if (this != &other) {
        reset(true);
        release_all_slabs();
        slab_size_ = other.slab_size_;
        slabs_ = std::move(other.slabs_);
        destructors_ = std::move(other.destructors_);
        total_used_ = other.total_used_;
        total_reserved_ = other.total_reserved_;
        other.total_used_ = 0;
        other.total_reserved_ = 0;
    }
    return *this;
}

void SlabArena::allocate_slab(size_t min_bytes) {
    size_t cap = page_align(std::max(slab_size_, min_bytes));
    void* ptr = mmap(nullptr, cap, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        throw std::bad_alloc();
    }
    slabs_.push_back(Slab{static_cast<uint8_t*>(ptr), cap, 0});
    total_reserved_ += cap;
}

void SlabArena::release_all_slabs() {
    for (const auto& slab : slabs_) {
        if (slab.base) {
            munmap(slab.base, slab.capacity);
        }
    }
    slabs_.clear();
    total_reserved_ = 0;
}

void* SlabArena::allocate(size_t bytes, size_t alignment) {
    if (bytes == 0) bytes = 1;
    if (alignment == 0) alignment = 1;

    if (!slabs_.empty()) {
        Slab& cur = slabs_.back();
        size_t aligned = (cur.offset + (alignment - 1)) & ~(alignment - 1);
        if (aligned + bytes <= cur.capacity) {
            void* result = cur.base + aligned;
            cur.offset = aligned + bytes;
            total_used_ += bytes;
            return result;
        }
    }

    allocate_slab(bytes + alignment);
    Slab& cur = slabs_.back();
    size_t aligned = (cur.offset + (alignment - 1)) & ~(alignment - 1);
    void* result = cur.base + aligned;
    cur.offset = aligned + bytes;
    total_used_ += bytes;
    return result;
}

std::string_view SlabArena::intern(std::string_view s) {
    if (s.empty()) return {};
    char* dst = static_cast<char*>(allocate(s.size() + 1, 1));
    std::memcpy(dst, s.data(), s.size());
    dst[s.size()] = '\0';
    return std::string_view(dst, s.size());
}

void SlabArena::reset(bool release_to_os) {
    // Destroy objects in reverse allocation order.
    for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
        it->fn(it->ptr);
    }
    destructors_.clear();

    if (release_to_os) {
        // Poison active bytes then unmap all slabs so RSS immediately drops.
        for (auto& slab : slabs_) {
            if (slab.base && slab.offset > 0) {
                poison_buffer(slab.base, std::min<size_t>(slab.offset, 4096));
                platform::release_pages(slab.base, slab.capacity);
            }
        }
        release_all_slabs();
    } else {
        for (auto& slab : slabs_) {
            if (slab.base && slab.offset > 0) {
                poison_buffer(slab.base, std::min<size_t>(slab.offset, 4096));
                platform::release_pages(slab.base, slab.capacity);
            }
            slab.offset = 0;
        }
    }
    total_used_ = 0;
}

} // namespace astra
