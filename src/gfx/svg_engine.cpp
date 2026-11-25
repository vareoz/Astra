#include "gfx/svg_engine.hpp"
#include "core/arena.hpp"
#include "dom/html_parser.hpp"
#include "style/css_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace astra {

namespace {

struct Pt {
    float x = 0.0f;
    float y = 0.0f;
};

struct SvgPaint {
    bool has_fill = true;
    uint32_t fill = 0xFF000000u;
    bool has_stroke = false;
    uint32_t stroke = 0x00000000u;
    float stroke_width = 1.0f;
    float opacity = 1.0f;
};

std::string trim_sv(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string lower_sv(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

float parse_num_attr(const DomNode* n, std::string_view name, float fallback = 0.0f) {
    if (!n) return fallback;
    std::string v = trim_sv(n->get_attr(name));
    if (v.empty()) return fallback;
    return std::strtof(v.c_str(), nullptr);
}

uint32_t mul_alpha(uint32_t argb, float op) {
    float a = static_cast<float>((argb >> 24) & 0xFFu) * std::clamp(op, 0.0f, 1.0f);
    uint32_t ai = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(a)), 0, 255));
    return (ai << 24) | (argb & 0x00FFFFFFu);
}

struct SsCanvas {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;

    SsCanvas(int width, int height) : w(width), h(height), px(static_cast<size_t>(width * height), 0u) {}

    void blend(int x, int y, uint32_t src) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint32_t sa = (src >> 24) & 0xFFu;
        if (sa == 0) return;
        uint32_t& dst = px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
        if (sa == 255) {
            dst = src;
            return;
        }
        uint32_t da = (dst >> 24) & 0xFFu;
        uint32_t sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
        uint32_t dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
        uint32_t inv_sa = 255u - sa;
        uint32_t out_a = sa + (da * inv_sa) / 255u;
        if (out_a == 0) {
            dst = 0;
            return;
        }
        uint32_t out_r = (sr * sa + dr * da * inv_sa / 255u) / out_a;
        uint32_t out_g = (sg * sa + dg * da * inv_sa / 255u) / out_a;
        uint32_t out_b = (sb * sa + db * da * inv_sa / 255u) / out_a;
        dst = (std::min(255u, out_a) << 24) |
              (std::min(255u, out_r) << 16) |
              (std::min(255u, out_g) << 8) |
              std::min(255u, out_b);
    }

    void fill_disc(float cx, float cy, float r, uint32_t col) {
        int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.0f)));
        int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + r + 1.0f)));
        int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.0f)));
        int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + r + 1.0f)));
        float r2 = r * r;
        for (int y = y0; y <= y1; ++y) {
            float dy = (static_cast<float>(y) + 0.5f) - cy;
            for (int x = x0; x <= x1; ++x) {
                float dx = (static_cast<float>(x) + 0.5f) - cx;
                if (dx * dx + dy * dy <= r2) blend(x, y, col);
            }
        }
    }

    void stroke_segment(Pt a, Pt b, float sw, uint32_t col) {
        float half = std::max(0.5f, sw * 0.5f);
        int x0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - half - 1.0f)));
        int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(a.x, b.x) + half + 1.0f)));
        int y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - half - 1.0f)));
        int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max(a.y, b.y) + half + 1.0f)));
        float vx = b.x - a.x;
        float vy = b.y - a.y;
        float len2 = vx * vx + vy * vy;
        float h2 = half * half;
        for (int y = y0; y <= y1; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                float px_f = static_cast<float>(x) + 0.5f;
                float t = 0.0f;
                if (len2 > 1e-6f) {
                    t = std::clamp(((px_f - a.x) * vx + (py - a.y) * vy) / len2, 0.0f, 1.0f);
                }
                float dx = px_f - (a.x + t * vx);
                float dy = py - (a.y + t * vy);
                if (dx * dx + dy * dy <= h2) blend(x, y, col);
            }
        }
    }

    void fill_polygon(const std::vector<Pt>& pts, uint32_t col) {
        if (pts.size() < 3) return;
        float min_y = pts[0].y, max_y = pts[0].y;
        for (const auto& p : pts) {
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
        int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
        int y1 = std::min(h - 1, static_cast<int>(std::ceil(max_y)));
        std::vector<float> xs;
        for (int y = y0; y <= y1; ++y) {
            float sy = static_cast<float>(y) + 0.5f;
            xs.clear();
            for (size_t i = 0; i < pts.size(); ++i) {
                Pt p1 = pts[i];
                Pt p2 = pts[(i + 1) % pts.size()];
                if ((p1.y <= sy && p2.y > sy) || (p2.y <= sy && p1.y > sy)) {
                    float t = (sy - p1.y) / (p2.y - p1.y);
                    xs.push_back(p1.x + t * (p2.x - p1.x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                int x0 = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5f)));
                int x1 = std::min(w - 1, static_cast<int>(std::floor(xs[i + 1] - 0.5f)));
                for (int x = x0; x <= x1; ++x) blend(x, y, col);
            }
        }
    }

    void fill_rounded_rect(float rx, float ry, float rw, float rh, float rad_x, float rad_y, uint32_t col) {
        if (rw <= 0.0f || rh <= 0.0f) return;
        float rad = std::min(std::max(rad_x, rad_y), std::min(rw, rh) * 0.5f);
        int x0 = std::max(0, static_cast<int>(std::floor(rx)));
        int x1 = std::min(w - 1, static_cast<int>(std::ceil(rx + rw)));
        int y0 = std::max(0, static_cast<int>(std::floor(ry)));
        int y1 = std::min(h - 1, static_cast<int>(std::ceil(ry + rh)));
        for (int y = y0; y <= y1; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            if (py < ry || py > ry + rh) continue;
            for (int x = x0; x <= x1; ++x) {
                float px_f = static_cast<float>(x) + 0.5f;
                if (px_f < rx || px_f > rx + rw) continue;
                bool inside = true;
                if (rad > 0.5f) {
                    float cx = std::clamp(px_f, rx + rad, rx + rw - rad);
                    float cy = std::clamp(py, ry + rad, ry + rh - rad);
                    float dx = px_f - cx, dy = py - cy;
                    if (dx * dx + dy * dy > rad * rad) inside = false;
                }
                if (inside) blend(x, y, col);
            }
        }
    }

    void stroke_rounded_rect(float rx, float ry, float rw, float rh, float rad, float sw, uint32_t col) {
        if (rw <= 0.0f || rh <= 0.0f) return;
        rad = std::min(rad, std::min(rw, rh) * 0.5f);
        float half = std::max(0.5f, sw * 0.5f);
        int x0 = std::max(0, static_cast<int>(std::floor(rx - half - 1.0f)));
        int x1 = std::min(w - 1, static_cast<int>(std::ceil(rx + rw + half + 1.0f)));
        int y0 = std::max(0, static_cast<int>(std::floor(ry - half - 1.0f)));
        int y1 = std::min(h - 1, static_cast<int>(std::ceil(ry + rh + half + 1.0f)));
        for (int y = y0; y <= y1; ++y) {
            float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                float px_f = static_cast<float>(x) + 0.5f;
                // Signed distance to rounded rectangle
                float cx = rx + rw * 0.5f;
                float cy = ry + rh * 0.5f;
                float qx = std::abs(px_f - cx) - (rw * 0.5f - rad);
                float qy = std::abs(py - cy) - (rh * 0.5f - rad);
                float ox = std::max(qx, 0.0f);
                float oy = std::max(qy, 0.0f);
                float sd = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
                if (std::abs(sd) <= half) blend(x, y, col);
            }
        }
    }

    void fill_ellipse(float cx, float cy, float rx, float ry, uint32_t col) {
        if (rx <= 0.0f || ry <= 0.0f) return;
        int x0 = std::max(0, static_cast<int>(std::floor(cx - rx - 1.0f)));
        int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + rx + 1.0f)));
        int y0 = std::max(0, static_cast<int>(std::floor(cy - ry - 1.0f)));
        int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + ry + 1.0f)));
        for (int y = y0; y <= y1; ++y) {
            float dy = ((static_cast<float>(y) + 0.5f) - cy) / ry;
            for (int x = x0; x <= x1; ++x) {
                float dx = ((static_cast<float>(x) + 0.5f) - cx) / rx;
                if (dx * dx + dy * dy <= 1.0f) blend(x, y, col);
            }
        }
    }

    void stroke_ellipse(float cx, float cy, float rx, float ry, float sw, uint32_t col) {
        if (rx <= 0.0f || ry <= 0.0f) return;
        const int segs = std::clamp(static_cast<int>(std::max(rx, ry) * 1.5f), 24, 128);
        Pt prev{cx + rx, cy};
        for (int i = 1; i <= segs; ++i) {
            float ang = (2.0f * 3.14159265f * static_cast<float>(i)) / static_cast<float>(segs);
            Pt cur{cx + std::cos(ang) * rx, cy + std::sin(ang) * ry};
            stroke_segment(prev, cur, sw, col);
            prev = cur;
        }
    }
};

std::vector<float> parse_number_list(std::string_view s) {
    std::vector<float> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        const char* start = s.data() + i;
        char* end = nullptr;
        float v = std::strtof(start, &end);
        if (end == start) {
            ++i;
            continue;
        }
        out.push_back(v);
        i = static_cast<size_t>(end - s.data());
    }
    return out;
}

struct SubPath {
    std::vector<Pt> pts;
    bool closed = false;
};

std::vector<SubPath> parse_svg_path(std::string_view d) {
    std::vector<SubPath> subpaths;
    SubPath cur;
    Pt pen{0.0f, 0.0f};
    Pt start_pt{0.0f, 0.0f};
    char cmd = 0;
    size_t i = 0;

    auto skip_sep = [&]() {
        while (i < d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) ++i;
    };
    auto has_num = [&]() -> bool {
        skip_sep();
        if (i >= d.size()) return false;
        char c = d[i];
        return std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.';
    };
    auto read_num = [&]() -> float {
        skip_sep();
        if (i >= d.size()) return 0.0f;
        const char* st = d.data() + i;
        char* en = nullptr;
        float v = std::strtof(st, &en);
        if (en == st) { ++i; return 0.0f; }
        i = static_cast<size_t>(en - d.data());
        return v;
    };

    while (i < d.size()) {
        skip_sep();
        if (i >= d.size()) break;
        if (std::isalpha(static_cast<unsigned char>(d[i]))) {
            cmd = d[i++];
        } else if (cmd == 0) {
            ++i;
            continue;
        }

        if (cmd == 'Z' || cmd == 'z') {
            if (!cur.pts.empty()) {
                cur.closed = true;
                subpaths.push_back(std::move(cur));
                cur = SubPath{};
            }
            pen = start_pt;
            cmd = 0;
            continue;
        }

        if (cmd == 'M' || cmd == 'm') {
            if (!cur.pts.empty()) {
                subpaths.push_back(std::move(cur));
                cur = SubPath{};
            }
            float x = read_num();
            float y = read_num();
            if (cmd == 'm') { pen.x += x; pen.y += y; }
            else { pen.x = x; pen.y = y; }
            start_pt = pen;
            cur.pts.push_back(pen);
            cmd = (cmd == 'm') ? 'l' : 'L';
            continue;
        }

        if (cmd == 'L' || cmd == 'l') {
            if (!has_num()) { cmd = 0; continue; }
            float x = read_num();
            float y = read_num();
            if (cmd == 'l') { pen.x += x; pen.y += y; }
            else { pen.x = x; pen.y = y; }
            cur.pts.push_back(pen);
        } else if (cmd == 'H' || cmd == 'h') {
            if (!has_num()) { cmd = 0; continue; }
            float x = read_num();
            pen.x = (cmd == 'h') ? (pen.x + x) : x;
            cur.pts.push_back(pen);
        } else if (cmd == 'V' || cmd == 'v') {
            if (!has_num()) { cmd = 0; continue; }
            float y = read_num();
            pen.y = (cmd == 'v') ? (pen.y + y) : y;
            cur.pts.push_back(pen);
        } else if (cmd == 'Q' || cmd == 'q') {
            if (!has_num()) { cmd = 0; continue; }
            float cx = read_num(), cy = read_num();
            float ex = read_num(), ey = read_num();
            if (cmd == 'q') { cx += pen.x; cy += pen.y; ex += pen.x; ey += pen.y; }
            Pt p0 = pen, p1{cx, cy}, p2{ex, ey};
            for (int s = 1; s <= 12; ++s) {
                float t = static_cast<float>(s) / 12.0f;
                float u = 1.0f - t;
                cur.pts.push_back(Pt{
                    u * u * p0.x + 2.0f * u * t * p1.x + t * t * p2.x,
                    u * u * p0.y + 2.0f * u * t * p1.y + t * t * p2.y
                });
            }
            pen = p2;
        } else if (cmd == 'C' || cmd == 'c') {
            if (!has_num()) { cmd = 0; continue; }
            float c1x = read_num(), c1y = read_num();
            float c2x = read_num(), c2y = read_num();
            float ex = read_num(), ey = read_num();
            if (cmd == 'c') {
                c1x += pen.x; c1y += pen.y;
                c2x += pen.x; c2y += pen.y;
                ex += pen.x; ey += pen.y;
            }
            Pt p0 = pen, p1{c1x, c1y}, p2{c2x, c2y}, p3{ex, ey};
            for (int s = 1; s <= 16; ++s) {
                float t = static_cast<float>(s) / 16.0f;
                float u = 1.0f - t;
                cur.pts.push_back(Pt{
                    u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
                    u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y
                });
            }
            pen = p3;
        } else {
            cmd = 0;
        }
    }
    if (!cur.pts.empty()) subpaths.push_back(std::move(cur));
    return subpaths;
}

SvgPaint derive_paint(const DomNode* node, const SvgPaint& parent) {
    SvgPaint p = parent;
    if (!node) return p;

    auto apply_kv = [&](std::string_view key, std::string_view val) {
        std::string k = lower_sv(trim_sv(key));
        std::string v = trim_sv(val);
        if (v.empty()) return;
        if (k == "fill") {
            if (lower_sv(v) == "none") {
                p.has_fill = false;
            } else {
                uint32_t c = 0;
                if (SvgEngine::parse_svg_color(v, c)) {
                    p.has_fill = true;
                    p.fill = c;
                }
            }
        } else if (k == "stroke") {
            if (lower_sv(v) == "none") {
                p.has_stroke = false;
            } else {
                uint32_t c = 0;
                if (SvgEngine::parse_svg_color(v, c)) {
                    p.has_stroke = true;
                    p.stroke = c;
                }
            }
        } else if (k == "stroke-width") {
            float sw = std::strtof(v.c_str(), nullptr);
            if (sw > 0.0f) p.stroke_width = sw;
        } else if (k == "opacity") {
            p.opacity *= std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        }
    };

    for (const auto& attr : node->attrs) {
        if (attr.name == "style") {
            size_t st = 0;
            while (st < attr.value.size()) {
                size_t semi = attr.value.find(';', st);
                std::string_view item = (semi == std::string::npos)
                                            ? std::string_view(attr.value).substr(st)
                                            : std::string_view(attr.value).substr(st, semi - st);
                size_t col = item.find(':');
                if (col != std::string_view::npos) {
                    apply_kv(item.substr(0, col), item.substr(col + 1));
                }
                if (semi == std::string::npos) break;
                st = semi + 1;
            }
        } else {
            apply_kv(attr.name, attr.value);
        }
    }
    return p;
}

void render_svg_node(const DomNode* node,
                     const SvgPaint& parent_paint,
                     float vb_x, float vb_y, float sx, float sy,
                     SsCanvas& canvas) {
    if (!node || node->type != NodeType::Element) return;

    // SVG 1.1 §5.6 y §14: el contenido de estos elementos son DEFINICIONES,
    // no se pinta directamente; sólo se referencia desde otros elementos. Sin
    // esta exclusión, el rectángulo que suele llevar dentro un <clipPath>
    // acababa dibujado y tapaba todo el lienzo: por eso logos y wordmarks
    // salían como bloques macizos en lugar de su forma real.
    static const char* const kNonRendered[] = {
        "defs", "clippath", "mask", "symbol", "marker", "pattern",
        "lineargradient", "radialgradient", "filter", "metadata", "title", "desc"
    };
    {
        std::string lt = node->tag;
        std::transform(lt.begin(), lt.end(), lt.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* nr : kNonRendered) {
            if (lt == nr) return;
        }
    }

    SvgPaint p = derive_paint(node, parent_paint);
    float scale_sw = (sx + sy) * 0.5f;
    float sw = std::max(1.0f, p.stroke_width * scale_sw);
    uint32_t fill_col = mul_alpha(p.fill, p.opacity);
    uint32_t stroke_col = mul_alpha(p.stroke, p.opacity);

    auto tx = [&](float x) { return (x - vb_x) * sx; };
    auto ty = [&](float y) { return (y - vb_y) * sy; };

    const std::string& tag = node->tag;
    if (tag == "rect") {
        float x = tx(parse_num_attr(node, "x", 0.0f));
        float y = ty(parse_num_attr(node, "y", 0.0f));
        float rw = parse_num_attr(node, "width", 0.0f) * sx;
        float rh = parse_num_attr(node, "height", 0.0f) * sy;
        float rx = parse_num_attr(node, "rx", 0.0f) * sx;
        float ry = parse_num_attr(node, "ry", rx / std::max(0.001f, sx)) * sy;
        if (p.has_fill) canvas.fill_rounded_rect(x, y, rw, rh, rx, ry, fill_col);
        if (p.has_stroke) canvas.stroke_rounded_rect(x, y, rw, rh, std::max(rx, ry), sw, stroke_col);
    } else if (tag == "circle") {
        float cx = tx(parse_num_attr(node, "cx", 0.0f));
        float cy = ty(parse_num_attr(node, "cy", 0.0f));
        float r = parse_num_attr(node, "r", 0.0f);
        float rx = r * sx;
        float ry = r * sy;
        if (p.has_fill) canvas.fill_ellipse(cx, cy, rx, ry, fill_col);
        if (p.has_stroke) canvas.stroke_ellipse(cx, cy, rx, ry, sw, stroke_col);
    } else if (tag == "ellipse") {
        float cx = tx(parse_num_attr(node, "cx", 0.0f));
        float cy = ty(parse_num_attr(node, "cy", 0.0f));
        float rx = parse_num_attr(node, "rx", 0.0f) * sx;
        float ry = parse_num_attr(node, "ry", 0.0f) * sy;
        if (p.has_fill) canvas.fill_ellipse(cx, cy, rx, ry, fill_col);
        if (p.has_stroke) canvas.stroke_ellipse(cx, cy, rx, ry, sw, stroke_col);
    } else if (tag == "line") {
        Pt a{tx(parse_num_attr(node, "x1", 0.0f)), ty(parse_num_attr(node, "y1", 0.0f))};
        Pt b{tx(parse_num_attr(node, "x2", 0.0f)), ty(parse_num_attr(node, "y2", 0.0f))};
        if (p.has_stroke) canvas.stroke_segment(a, b, sw, stroke_col);
    } else if (tag == "polyline" || tag == "polygon") {
        auto nums = parse_number_list(node->get_attr("points"));
        std::vector<Pt> pts;
        for (size_t i = 0; i + 1 < nums.size(); i += 2) {
            pts.push_back(Pt{tx(nums[i]), ty(nums[i + 1])});
        }
        if (p.has_fill && pts.size() >= 3) canvas.fill_polygon(pts, fill_col);
        if (p.has_stroke && pts.size() >= 2) {
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                canvas.stroke_segment(pts[i], pts[i + 1], sw, stroke_col);
            }
            if (tag == "polygon" && pts.size() >= 3) {
                canvas.stroke_segment(pts.back(), pts.front(), sw, stroke_col);
            }
        }
    } else if (tag == "path") {
        auto subpaths = parse_svg_path(node->get_attr("d"));
        for (auto& sp : subpaths) {
            for (auto& pt : sp.pts) {
                pt.x = tx(pt.x);
                pt.y = ty(pt.y);
            }
            if (p.has_fill && sp.pts.size() >= 3) canvas.fill_polygon(sp.pts, fill_col);
            if (p.has_stroke && sp.pts.size() >= 2) {
                for (size_t i = 0; i + 1 < sp.pts.size(); ++i) {
                    canvas.stroke_segment(sp.pts[i], sp.pts[i + 1], sw, stroke_col);
                }
                if (sp.closed && sp.pts.size() >= 3) {
                    canvas.stroke_segment(sp.pts.back(), sp.pts.front(), sw, stroke_col);
                }
            }
        }
    }

    for (const DomNode* ch : node->children) {
        render_svg_node(ch, p, vb_x, vb_y, sx, sy, canvas);
    }
}

const DomNode* find_svg_root(const DomNode* n) {
    if (!n) return nullptr;
    if (n->type == NodeType::Element && n->tag == "svg") return n;
    for (const DomNode* ch : n->children) {
        if (const DomNode* found = find_svg_root(ch)) return found;
    }
    return nullptr;
}

} // namespace

bool SvgEngine::parse_svg_color(std::string_view s, uint32_t& out_argb) {
    std::string t = lower_sv(trim_sv(s));
    if (t.empty() || t == "none" || t == "transparent") {
        out_argb = 0x00000000u;
        return t == "transparent";
    }
    Color fallback{0, 0, 0, 0};
    Color c = StyleEngine::parse_color(t, fallback);
    if (c.a == 0 && t != "transparent") return false;
    out_argb = c.to_argb();
    return true;
}

SvgSurface SvgEngine::render_svg(std::string_view svg_xml, int target_w, int target_h) {
    SvgSurface out;
    if (svg_xml.empty()) return out;

    SlabArena arena(32 * 1024);
    HtmlDocument doc = HtmlDocument::parse(arena, svg_xml);
    const DomNode* svg = find_svg_root(doc.root());
    if (!svg) return out;

    float attr_w = parse_num_attr(svg, "width", 0.0f);
    float attr_h = parse_num_attr(svg, "height", 0.0f);
    float vb_x = 0.0f, vb_y = 0.0f, vb_w = attr_w > 0 ? attr_w : 100.0f, vb_h = attr_h > 0 ? attr_h : 100.0f;

    std::string vb = svg->get_attr("viewbox");
    if (vb.empty()) vb = svg->get_attr("viewBox");
    if (!vb.empty()) {
        auto nums = parse_number_list(vb);
        if (nums.size() >= 4 && nums[2] > 0.0f && nums[3] > 0.0f) {
            vb_x = nums[0];
            vb_y = nums[1];
            vb_w = nums[2];
            vb_h = nums[3];
        }
    }

    int w = target_w > 0 ? target_w : static_cast<int>(std::lround(attr_w > 0 ? attr_w : vb_w));
    int h = target_h > 0 ? target_h : static_cast<int>(std::lround(attr_h > 0 ? attr_h : vb_h));
    w = std::clamp(w, 8, kMaxDim);
    h = std::clamp(h, 8, kMaxDim);

    // 2x2 supersampled anti-aliasing canvas
    constexpr int kSS = 2;
    SsCanvas ss(w * kSS, h * kSS);
    float sx = static_cast<float>(ss.w) / std::max(1e-3f, vb_w);
    float sy = static_cast<float>(ss.h) / std::max(1e-3f, vb_h);

    SvgPaint root_paint;
    render_svg_node(svg, root_paint, vb_x, vb_y, sx, sy, ss);

    // Downsample 2x2 box filter into final ARGB32 surface
    out.width = w;
    out.height = h;
    out.argb.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
            for (int dy = 0; dy < kSS; ++dy) {
                for (int dx = 0; dx < kSS; ++dx) {
                    uint32_t c = ss.px[static_cast<size_t>(y * kSS + dy) * static_cast<size_t>(ss.w) +
                                       static_cast<size_t>(x * kSS + dx)];
                    uint32_t a = (c >> 24) & 0xFFu;
                    sum_a += a;
                    sum_r += ((c >> 16) & 0xFFu) * a;
                    sum_g += ((c >> 8) & 0xFFu) * a;
                    sum_b += (c & 0xFFu) * a;
                }
            }
            uint32_t out_a = sum_a / 4u;
            if (out_a > 0 && sum_a > 0) {
                uint32_t out_r = sum_r / sum_a;
                uint32_t out_g = sum_g / sum_a;
                uint32_t out_b = sum_b / sum_a;
                out.argb[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                    (std::min(255u, out_a) << 24) |
                    (std::min(255u, out_r) << 16) |
                    (std::min(255u, out_g) << 8) |
                    std::min(255u, out_b);
            }
        }
    }
    out.valid = true;
    return out;
}

} // namespace astra
