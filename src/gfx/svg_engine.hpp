#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

struct SvgSurface {
    bool valid = false;
    int width = 0;
    int height = 0;
    std::vector<uint32_t> argb;
};

// AstraSVG v1: Native zero-dependency C++20 inline <svg> vector graphics engine.
// Supports viewBox scaling, <g> attribute inheritance, <rect> (with rx/ry),
// <circle>, <ellipse>, <line>, <polyline>, <polygon>, and <path>
// (M/m, L/l, H/h, V/v, Q/q quadratic Bezier, C/c cubic Bezier, Z/z)
// with 2x2 supersampled anti-aliasing and a strict 512x512 surface cap.
class SvgEngine {
public:
    static constexpr int kMaxDim = 512;

    // Render SVG XML markup into an ARGB32 surface of (target_w x target_h).
    // If target_w or target_h <= 0, derives dimensions from SVG width/height/viewBox.
    static SvgSurface render_svg(std::string_view svg_xml, int target_w = 0, int target_h = 0);

    // Parse SVG color string (#rgb, #rrggbb, rgb/rgba, named CSS colors, "none")
    static bool parse_svg_color(std::string_view s, uint32_t& out_argb);
};

} // namespace astra
