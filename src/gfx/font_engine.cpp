#include "gfx/font_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>

namespace astra {

namespace {

// Verified x86_64 FreeType 2 ABI offsets on libfreetype.so.6
constexpr size_t kFtFaceGlyphOffset = 152;
constexpr size_t kFtSlotAdvanceXOffset = 128;
constexpr size_t kFtSlotFormatOffset = 144;
constexpr size_t kFtSlotBitmapRowsOffset = 152;
constexpr size_t kFtSlotBitmapWidthOffset = 156;
constexpr size_t kFtSlotBitmapPitchOffset = 160;
constexpr size_t kFtSlotBitmapBufferOffset = 168;
constexpr size_t kFtSlotPixelModeOffset = 178;
constexpr size_t kFtSlotBitmapLeftOffset = 192;
constexpr size_t kFtSlotBitmapTopOffset = 196;

constexpr uint32_t kFtGlyphFormatBitmap = 0x62697473u; // 'bits'
constexpr int kFtLoadRender = 4;
constexpr uint8_t kFtPixelModeGray = 2;

template <typename T>
T read_at(const void* base, size_t byte_offset) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(base) + byte_offset, sizeof(T));
    return v;
}

void* try_load_face(void* lib,
                    int (*ft_new_face)(void*, const char*, long, void**),
                    const std::vector<const char*>& candidates) {
    for (const char* path : candidates) {
        void* face = nullptr;
        if (ft_new_face(lib, path, 0, &face) == 0 && face != nullptr) {
            return face;
        }
    }
    return nullptr;
}

int fallback_glyph_scale(float font_size) {
    if (font_size >= 26.0f) return 3;
    if (font_size >= 18.0f) return 2;
    return 1;
}

} // namespace

FontEngine& FontEngine::instance() {
    static FontEngine eng;
    return eng;
}

FontEngine::FontEngine() {
    dl_handle_ = dlopen("libfreetype.so.6", RTLD_LAZY);
    if (!dl_handle_) dl_handle_ = dlopen("libfreetype.so", RTLD_LAZY);
    if (!dl_handle_) return;

    ft_init_ = reinterpret_cast<FT_Init_FreeType_fn>(dlsym(dl_handle_, "FT_Init_FreeType"));
    ft_done_ = reinterpret_cast<FT_Done_FreeType_fn>(dlsym(dl_handle_, "FT_Done_FreeType"));
    ft_new_face_ = reinterpret_cast<FT_New_Face_fn>(dlsym(dl_handle_, "FT_New_Face"));
    ft_done_face_ = reinterpret_cast<FT_Done_Face_fn>(dlsym(dl_handle_, "FT_Done_Face"));
    ft_set_pixel_sizes_ = reinterpret_cast<FT_Set_Pixel_Sizes_fn>(dlsym(dl_handle_, "FT_Set_Pixel_Sizes"));
    ft_load_char_ = reinterpret_cast<FT_Load_Char_fn>(dlsym(dl_handle_, "FT_Load_Char"));

    if (!ft_init_ || !ft_done_ || !ft_new_face_ || !ft_done_face_ ||
        !ft_set_pixel_sizes_ || !ft_load_char_) {
        return;
    }

    if (ft_init_(&ft_lib_) != 0 || !ft_lib_) return;

    face_sans_reg_ = try_load_face(ft_lib_, ft_new_face_, {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
    });
    face_sans_bold_ = try_load_face(ft_lib_, ft_new_face_, {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"
    });
    face_mono_reg_ = try_load_face(ft_lib_, ft_new_face_, {
        "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    });
    face_mono_bold_ = try_load_face(ft_lib_, ft_new_face_, {
        "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
    });

    if (!face_sans_reg_) return;

    // Runtime ABI sanity check on 'A' at 16px to guarantee safe glyph slot layout
    if (ft_set_pixel_sizes_(face_sans_reg_, 0, 16) == 0 &&
        ft_load_char_(face_sans_reg_, 'A', kFtLoadRender) == 0) {
        const void* slot = read_at<const void*>(face_sans_reg_, kFtFaceGlyphOffset);
        if (slot) {
            uint32_t fmt = read_at<uint32_t>(slot, kFtSlotFormatOffset);
            uint8_t pmode = read_at<uint8_t>(slot, kFtSlotPixelModeOffset);
            uint32_t rows = read_at<uint32_t>(slot, kFtSlotBitmapRowsOffset);
            uint32_t width = read_at<uint32_t>(slot, kFtSlotBitmapWidthOffset);
            if (fmt == kFtGlyphFormatBitmap && pmode == kFtPixelModeGray &&
                rows >= 6 && rows <= 32 && width >= 4 && width <= 32) {
                active_ = true;
            }
        }
    }
}

FontEngine::~FontEngine() {
    if (face_sans_reg_ && ft_done_face_) ft_done_face_(face_sans_reg_);
    if (face_sans_bold_ && ft_done_face_) ft_done_face_(face_sans_bold_);
    if (face_mono_reg_ && ft_done_face_) ft_done_face_(face_mono_reg_);
    if (face_mono_bold_ && ft_done_face_) ft_done_face_(face_mono_bold_);
    if (ft_lib_ && ft_done_) ft_done_(ft_lib_);
    if (dl_handle_) dlclose(dl_handle_);
}

uint32_t FontEngine::next_utf8_codepoint(std::string_view s, size_t& index) {
    if (index >= s.size()) return 0;
    uint8_t c0 = static_cast<uint8_t>(s[index++]);
    if (c0 < 0x80) return c0;
    if ((c0 & 0xE0) == 0xC0 && index < s.size()) {
        uint8_t c1 = static_cast<uint8_t>(s[index++]);
        return ((c0 & 0x1Fu) << 6) | (c1 & 0x3Fu);
    }
    if ((c0 & 0xF0) == 0xE0 && index + 1 < s.size()) {
        uint8_t c1 = static_cast<uint8_t>(s[index++]);
        uint8_t c2 = static_cast<uint8_t>(s[index++]);
        return ((c0 & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
    }
    if ((c0 & 0xF8) == 0xF0 && index + 2 < s.size()) {
        uint8_t c1 = static_cast<uint8_t>(s[index++]);
        uint8_t c2 = static_cast<uint8_t>(s[index++]);
        uint8_t c3 = static_cast<uint8_t>(s[index++]);
        return ((c0 & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
    }
    return '?';
}

void FontEngine::evict_if_needed_locked() {
    while (cache_bytes_ > kMaxCacheBytes && !cache_.empty()) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.last_used_tick < oldest->second.last_used_tick) {
                oldest = it;
            }
        }
        cache_bytes_ -= oldest->second.alpha.size() + sizeof(CachedGlyph);
        cache_.erase(oldest);
    }
}

const CachedGlyph* FontEngine::get_glyph(uint32_t codepoint, int pixel_size, bool bold, bool mono) {
    if (!active_) return nullptr;
    int px = std::clamp(pixel_size, 8, 72);
    uint8_t sbits = static_cast<uint8_t>((bold ? 1 : 0) | (mono ? 2 : 0));
    GlyphKey key{codepoint, static_cast<uint16_t>(px), sbits};

    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.last_used_tick = ++tick_;
        return &it->second;
    }

    void* face = nullptr;
    if (mono) {
        face = bold ? (face_mono_bold_ ? face_mono_bold_ : face_mono_reg_)
                    : (face_mono_reg_ ? face_mono_reg_ : face_sans_reg_);
    } else {
        face = bold ? (face_sans_bold_ ? face_sans_bold_ : face_sans_reg_)
                    : face_sans_reg_;
    }
    if (!face) face = face_sans_reg_;

    if (ft_set_pixel_sizes_(face, 0, static_cast<unsigned int>(px)) != 0) return nullptr;
    if (ft_load_char_(face, codepoint, kFtLoadRender) != 0) return nullptr;

    const void* slot = read_at<const void*>(face, kFtFaceGlyphOffset);
    if (!slot) return nullptr;

    uint32_t fmt = read_at<uint32_t>(slot, kFtSlotFormatOffset);
    if (fmt != kFtGlyphFormatBitmap) return nullptr;

    uint32_t rows = read_at<uint32_t>(slot, kFtSlotBitmapRowsOffset);
    uint32_t width = read_at<uint32_t>(slot, kFtSlotBitmapWidthOffset);
    int32_t pitch = read_at<int32_t>(slot, kFtSlotBitmapPitchOffset);
    const uint8_t* buf = read_at<const uint8_t*>(slot, kFtSlotBitmapBufferOffset);
    int32_t bleft = read_at<int32_t>(slot, kFtSlotBitmapLeftOffset);
    int32_t btop = read_at<int32_t>(slot, kFtSlotBitmapTopOffset);
    int64_t adv_x_26_6 = read_at<int64_t>(slot, kFtSlotAdvanceXOffset);

    CachedGlyph g;
    g.width = static_cast<int>(width);
    g.height = static_cast<int>(rows);
    g.bearing_x = bleft;
    g.bearing_y = btop;
    g.advance_x = std::max(1, static_cast<int>((adv_x_26_6 + 32) >> 6));
    g.last_used_tick = ++tick_;

    if (buf && width > 0 && rows > 0 && width <= 256 && rows <= 256) {
        g.alpha.resize(static_cast<size_t>(width) * rows, 0);
        int abs_pitch = std::abs(pitch);
        for (uint32_t r = 0; r < rows; ++r) {
            const uint8_t* src_row = (pitch >= 0)
                                         ? (buf + static_cast<size_t>(r) * abs_pitch)
                                         : (buf + static_cast<size_t>(rows - 1 - r) * abs_pitch);
            std::memcpy(g.alpha.data() + static_cast<size_t>(r) * width, src_row, width);
        }
    }

    cache_bytes_ += g.alpha.size() + sizeof(CachedGlyph);
    auto [ins_it, ok] = cache_.emplace(key, std::move(g));
    evict_if_needed_locked();
    return &ins_it->second;
}

float FontEngine::measure_text(std::string_view utf8, float font_size, bool bold, bool mono) {
    if (!active_) {
        int scale = fallback_glyph_scale(font_size);
        float adv = static_cast<float>(scale * 8 + (bold ? 1 : 0));
        return static_cast<float>(utf8.size()) * adv;
    }
    int px = static_cast<int>(std::round(std::clamp(font_size, 8.0f, 72.0f)));
    float total = 0.0f;
    size_t idx = 0;
    while (idx < utf8.size()) {
        uint32_t cp = next_utf8_codepoint(utf8, idx);
        const CachedGlyph* g = get_glyph(cp, px, bold, mono);
        if (g) total += static_cast<float>(g->advance_x);
        else total += static_cast<float>(px) * 0.5f;
    }
    return total;
}

float FontEngine::line_height(float font_size) const {
    if (!active_) {
        int scale = fallback_glyph_scale(font_size);
        return static_cast<float>(scale * 8) * 1.65f;
    }
    return std::ceil(std::clamp(font_size, 8.0f, 72.0f) * 1.48f);
}

size_t FontEngine::cache_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_bytes_;
}

size_t FontEngine::cached_glyph_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_.size();
}

} // namespace astra
