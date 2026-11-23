#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

struct CachedGlyph {
    int width = 0;
    int height = 0;
    int bearing_x = 0;
    int bearing_y = 0;
    int advance_x = 0;
    std::vector<uint8_t> alpha; // 8-bit anti-aliased coverage (width * height)
    uint64_t last_used_tick = 0;
};

// AstraFont: Zero-build-dependency TrueType vector typography engine.
// Dynamically loads libfreetype.so.6 via dlopen at runtime (no -lfreetype link flag),
// renders anti-aliased UTF-8 glyphs from system DejaVuSans & JetBrainsMono .ttf faces,
// and caches rasterized bitmaps in a bounded 4 MiB LRU cache.
// Falls back gracefully to built-in 8x8 bitmap metrics if FreeType is unavailable.
class FontEngine {
public:
    static constexpr size_t kMaxCacheBytes = 4ULL * 1024 * 1024;

    static FontEngine& instance();

    bool freetype_active() const { return active_; }

    // Fetch or rasterize an anti-aliased glyph (returns nullptr if FreeType inactive).
    const CachedGlyph* get_glyph(uint32_t codepoint, int pixel_size, bool bold, bool mono = false);

    // Exact UTF-8 text width in pixels (shared by LayoutEngine & Framebuffer).
    float measure_text(std::string_view utf8, float font_size, bool bold, bool mono = false);

    // Line height in pixels for a given font size.
    float line_height(float font_size) const;

    // Decode next UTF-8 codepoint and advance index.
    static uint32_t next_utf8_codepoint(std::string_view s, size_t& index);

    size_t cache_bytes() const;
    size_t cached_glyph_count() const;

private:
    FontEngine();
    ~FontEngine();

    struct GlyphKey {
        uint32_t codepoint = 0;
        uint16_t px_size = 14;
        uint8_t style_bits = 0; // bit 0: bold, bit 1: mono

        bool operator==(const GlyphKey& o) const {
            return codepoint == o.codepoint && px_size == o.px_size && style_bits == o.style_bits;
        }
    };

    struct GlyphKeyHash {
        size_t operator()(const GlyphKey& k) const {
            return (static_cast<size_t>(k.codepoint) * 2654435761ULL) ^
                   (static_cast<size_t>(k.px_size) << 16) ^
                   static_cast<size_t>(k.style_bits);
        }
    };

    void evict_if_needed_locked();

    bool active_ = false;
    void* dl_handle_ = nullptr;
    void* ft_lib_ = nullptr;
    void* face_sans_reg_ = nullptr;
    void* face_sans_bold_ = nullptr;
    void* face_mono_reg_ = nullptr;
    void* face_mono_bold_ = nullptr;

    using FT_Init_FreeType_fn = int (*)(void**);
    using FT_Done_FreeType_fn = int (*)(void*);
    using FT_New_Face_fn = int (*)(void*, const char*, long, void**);
    using FT_Done_Face_fn = int (*)(void*);
    using FT_Set_Pixel_Sizes_fn = int (*)(void*, unsigned int, unsigned int);
    using FT_Load_Char_fn = int (*)(void*, unsigned long, int);

    FT_Init_FreeType_fn ft_init_ = nullptr;
    FT_Done_FreeType_fn ft_done_ = nullptr;
    FT_New_Face_fn ft_new_face_ = nullptr;
    FT_Done_Face_fn ft_done_face_ = nullptr;
    FT_Set_Pixel_Sizes_fn ft_set_pixel_sizes_ = nullptr;
    FT_Load_Char_fn ft_load_char_ = nullptr;

    mutable std::mutex mu_;
    std::unordered_map<GlyphKey, CachedGlyph, GlyphKeyHash> cache_;
    size_t cache_bytes_ = 0;
    uint64_t tick_ = 0;
};

} // namespace astra
