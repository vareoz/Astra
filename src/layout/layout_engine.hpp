#pragma once

#include "core/arena.hpp"
#include "dom/html_parser.hpp"
#include "style/animation_engine.hpp"
#include "style/css_engine.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace astra {

struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct LayoutFrame {
    const DomNode* node = nullptr;
    ComputedStyle style;
    Rect border_box;
    Rect content_box;
    std::vector<std::string> wrapped_lines;
    std::vector<LayoutFrame*> children;
};

enum class DisplayItemType : uint32_t {
    FillRect = 1,
    StrokeRect = 2,
    DrawText = 3,
    Embed3D = 4,
    EmbedVideo = 5,
    DrawImage = 6,
    DrawInput = 7,
    DropShadow = 8,
    EmbedAudio = 9,
    FillGradient = 10,
    EmbedCanvas = 11
};

struct DisplayItem {
    DisplayItemType type = DisplayItemType::FillRect;
    Rect rect;
    uint32_t color_argb = 0xFFFFFFFF;
    uint32_t color2_argb = 0xFFFFFFFF;
    float angle_deg = 180.0f;
    float radius = 0.0f;
    float thickness = 1.0f;
    float font_size = 14.0f;
    int32_t z_index = 0;
    bool bold = false;
    bool monospace = false;
    bool focused = false;
    bool fixed_viewport = false;
    bool has_clip = false;
    Rect clip_rect;
    // Session 8: matriz afín 2D [a c e; b d f] ya resuelta alrededor del centro
    // de la caja, lista para que el compositor la aplique por píxel.
    bool has_transform = false;
    float xform[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::string text;
};

enum class HitRegionKind : uint32_t {
    Link = 1,
    Button = 2,
    Scene3D = 3,
    Video = 4,
    Input = 5,
    Audio = 6,
    Download = 7
};

struct HitRegion {
    HitRegionKind kind = HitRegionKind::Link;
    Rect rect;
    int32_t z_index = 0;
    bool fixed_viewport = false;
    std::string target; // href for Link/Download, id/selector for Button/Input, src for Video/Audio/3D
};

struct DisplayList {
    uint32_t viewport_width = 1280;
    uint32_t viewport_height = 720;
    float content_height = 0.0f;
    uint32_t clear_color_argb = 0xFF0D1118;
    std::vector<DisplayItem> items;
    std::vector<HitRegion> hit_regions;

    const HitRegion* hit_test(float x, float y, float scroll_y = 0.0f) const;

    std::vector<uint8_t> serialize() const;
    static bool deserialize(const std::vector<uint8_t>& blob, DisplayList& out);
};

class LayoutEngine {
public:
    // `timeline` es opcional: cuando se pasa, el motor registra en él las
    // animaciones y transiciones declaradas por cada elemento y aplica el
    // efecto animado vigente sobre la cascada (Session 8).
    static LayoutFrame* build_layout(SlabArena& arena,
                                     const HtmlDocument& doc,
                                     const CssStylesheet& sheet,
                                     float viewport_w,
                                     float viewport_h,
                                     AnimationTimeline* timeline = nullptr);

    static DisplayList build_display_list(const LayoutFrame* root,
                                          uint32_t viewport_w,
                                          uint32_t viewport_h);
};

} // namespace astra
