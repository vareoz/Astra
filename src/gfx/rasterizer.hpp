#pragma once

#include "gfx/astra3d.hpp"
#include "gfx/image_decoder.hpp"
#include "layout/layout_engine.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

struct ChromeStatus {
    std::string active_url;
    std::string active_title;
    std::vector<std::string> tab_titles;
    size_t active_tab_index = 0;
    uint64_t rss_mb = 0;
    uint64_t ceiling_mb = 2048;
    uint64_t trackers_blocked = 0;
    size_t extensions_loaded = 0;
    bool tls_verified = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool address_bar_focused = false;
    std::string address_bar_text;
};

struct VideoBlitFrame {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> argb;
    std::string label;
    float progress = 0.0f;
    std::string time_label;
    bool has_audio = false;
};

struct AudioBlitCard {
    bool valid = false;
    std::string label;
    std::string meta;
    std::vector<float> waveform_peaks;
};

class Framebuffer;

struct InteractiveCallbacks {
    std::function<void(int x, int y)> on_click;
    std::function<void(int delta_y)> on_scroll;
    std::function<void(const std::string& text)> on_text;
    std::function<void()> on_backspace;
    std::function<void()> on_enter;
    std::function<void(Framebuffer& fb)> on_repaint;
};

// Astra Compositor v5: ARGB32 software compositor with Anti-Aliased TrueType (AstraFont),
// WebRender-style per-item overflow:hidden pixel clipping, z-index layering, position:fixed pinning,
// soft DropShadow, interactive Chrome v4, APZ scroll_y, native PNG/JPEG/BMP/PPM <img> blitting,
// interactive <input>, smooth Astra3D, interactive <video> scrubber + <audio> waveform card,
// zero-dep PNG encoder, and interactive X11 window.
class Framebuffer {
public:
    static constexpr int kChromeHeight = 84;

    Framebuffer(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t* pixels() { return pixels_.data(); }
    const uint32_t* pixels() const { return pixels_.data(); }

    void set_clip_rect(int x, int y, int w, int h);
    void clear_clip_rect();

    void clear(uint32_t argb);
    void blend_pixel(int x, int y, uint32_t argb);
    void fill_rect(int x, int y, int w, int h, uint32_t argb);
    void fill_rounded_rect(int x, int y, int w, int h, int radius, uint32_t argb);
    void stroke_rounded_rect(int x, int y, int w, int h, int radius, int thickness, uint32_t argb);
    void draw_text(int x, int y, std::string_view text, uint32_t argb, float font_size,
                   bool bold = false, bool monospace = false);
    void blit_scaled(int dst_x, int dst_y, int dst_w, int dst_h,
                     const uint32_t* src, int src_w, int src_h);

    int draw_browser_chrome(const ChromeStatus& status);

    using VideoProvider = std::function<bool(const std::string& src, VideoBlitFrame& out)>;
    using ImageProvider = std::function<bool(const std::string& src, DecodedImage& out)>;
    using AudioProvider = std::function<bool(const std::string& src, AudioBlitCard& out)>;

    void composite_display_list(const DisplayList& dl,
                                int y_offset,
                                float scroll_y = 0.0f,
                                const VideoProvider& video_provider = nullptr,
                                const ImageProvider& image_provider = nullptr,
                                const Camera3D& cam3d = Camera3D{},
                                float time3d_sec = 0.72f,
                                const AudioProvider& audio_provider = nullptr);

    bool save_png(const std::string& path) const;

    // Interactive Linux X11 desktop window with live Expose/Click/Scroll/Key loop.
    bool present_x11_interactive(const std::string& window_title,
                                 const InteractiveCallbacks& callbacks);

private:
    int width_;
    int height_;
    bool has_clip_ = false;
    int clip_x0_ = 0;
    int clip_y0_ = 0;
    int clip_x1_ = 0;
    int clip_y1_ = 0;
    std::vector<uint32_t> pixels_;
};

} // namespace astra
