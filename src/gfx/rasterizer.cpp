#include "gfx/rasterizer.hpp"

#include "core/platform.hpp"
#include "gfx/font_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <unistd.h>

namespace astra {

namespace {

const uint8_t* glyph_bitmap(unsigned char ch) {
    static const uint8_t kFont[95][8] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
        {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
        {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
        {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
        {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
        {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
        {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
        {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
        {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
        {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
        {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
        {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
        {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
        {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
        {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
        {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
        {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
        {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
        {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
        {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
        {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
        {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
        {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
        {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
        {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
        {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
        {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
        {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
        {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
        {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
        {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
        {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
        {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
        {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
        {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
        {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
        {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
        {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
        {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
        {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
        {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
        {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
        {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
        {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
        {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
        {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
        {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}
    };
    if (ch >= 32 && ch <= 126) return kFont[ch - 32];
    return kFont['?' - 32];
}

std::string utf8_to_printable(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 128) {
            out.push_back(static_cast<char>(c));
        } else if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char n = static_cast<unsigned char>(s[++i]);
            switch (n) {
                case 0xA1: out.push_back('a'); break; case 0xA9: out.push_back('e'); break;
                case 0xAD: out.push_back('i'); break; case 0xB3: out.push_back('o'); break;
                case 0xBA: out.push_back('u'); break; case 0xB1: out.push_back('n'); break;
                case 0x81: out.push_back('A'); break; case 0x89: out.push_back('E'); break;
                case 0x8D: out.push_back('I'); break; case 0x93: out.push_back('O'); break;
                case 0x9A: out.push_back('U'); break; case 0x91: out.push_back('N'); break;
                default: out.push_back('?'); break;
            }
        } else if (c == 0xC2 && i + 1 < s.size()) { ++i; out.push_back(' '); }
        else if (c == 0xE2 && i + 2 < s.size()) { i += 2; out.push_back('-'); }
    }
    return out;
}

uint32_t crc32_compute(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32_compute(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void write_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void write_png_chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    write_be32(out, static_cast<uint32_t>(data.size()));
    size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32_compute(out.data() + crc_start, 4 + data.size());
    write_be32(out, crc);
}

bool inside_rounded(int x, int y, int w, int h, int r) {
    if (r <= 0) return true;
    r = std::min({r, w / 2, h / 2});
    int dx = 0, dy = 0;
    if (x < r && y < r) { dx = r - x; dy = r - y; }
    else if (x >= w - r && y < r) { dx = x - (w - r - 1); dy = r - y; }
    else if (x < r && y >= h - r) { dx = r - x; dy = y - (h - r - 1); }
    else if (x >= w - r && y >= h - r) { dx = x - (w - r - 1); dy = y - (h - r - 1); }
    else return true;
    return (dx * dx + dy * dy) <= r * r;
}

} // namespace

Framebuffer::Framebuffer(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width * height), 0xFF0D1118u) {}

void Framebuffer::set_clip_rect(int x, int y, int w, int h) {
    has_clip_ = true;
    clip_x0_ = std::max(0, x);
    clip_y0_ = std::max(0, y);
    clip_x1_ = std::min(width_, x + std::max(0, w));
    clip_y1_ = std::min(height_, y + std::max(0, h));
}

void Framebuffer::clear_clip_rect() {
    has_clip_ = false;
}

void Framebuffer::clear(uint32_t argb) {
    std::fill(pixels_.begin(), pixels_.end(), argb);
}

void Framebuffer::blend_pixel(int x, int y, uint32_t argb) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    if (has_clip_ && (x < clip_x0_ || x >= clip_x1_ || y < clip_y0_ || y >= clip_y1_)) return;
    uint32_t a = (argb >> 24) & 0xFFu;
    if (a == 0) return;
    uint32_t& dst = pixels_[static_cast<size_t>(y * width_ + x)];
    if (a == 255) { dst = argb; return; }
    uint32_t inv = 255 - a;
    uint32_t sr = (argb >> 16) & 0xFFu, sg = (argb >> 8) & 0xFFu, sb = argb & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
    dst = 0xFF000000u | (((sr * a + dr * inv) / 255) << 16) |
          (((sg * a + dg * inv) / 255) << 8) | ((sb * a + db * inv) / 255);
}

void Framebuffer::fill_rect(int x, int y, int w, int h, uint32_t argb) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) blend_pixel(px, py, argb);
    }
}

void Framebuffer::fill_rounded_rect(int x, int y, int w, int h, int radius, uint32_t argb) {
    if (radius <= 0) { fill_rect(x, y, w, h, argb); return; }
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            if (inside_rounded(px - x, py - y, w, h, radius)) blend_pixel(px, py, argb);
        }
    }
}

namespace {

// Color de un linear-gradient CSS en coordenadas locales (float), con la misma
// convención de ángulo que fill_gradient_rounded_rect (0deg = arriba).
uint32_t gradient_color_at(float lx, float ly, float w, float h,
                           uint32_t start_argb, uint32_t end_argb, float angle_deg) {
    constexpr float kPi = 3.14159265358979323846f;
    const float rad = angle_deg * (kPi / 180.0f);
    const float vx = std::sin(rad);
    const float vy = -std::cos(rad);
    const float p10 = w * vx;
    const float p01 = h * vy;
    const float p11 = p10 + p01;
    const float min_p = std::min({0.0f, p10, p01, p11});
    const float max_p = std::max({0.0f, p10, p01, p11});
    const float inv_span = (max_p - min_p > 1e-4f) ? (1.0f / (max_p - min_p)) : 0.0f;
    const float t = std::clamp((lx * vx + ly * vy - min_p) * inv_span, 0.0f, 1.0f);

    auto mix = [t](uint32_t a, uint32_t b, int shift) {
        const float va = static_cast<float>((a >> shift) & 0xFFu);
        const float vb = static_cast<float>((b >> shift) & 0xFFu);
        return static_cast<uint32_t>(std::lround(va + (vb - va) * t)) & 0xFFu;
    };
    return (mix(start_argb, end_argb, 24) << 24) | (mix(start_argb, end_argb, 16) << 16) |
           (mix(start_argb, end_argb, 8) << 8) | mix(start_argb, end_argb, 0);
}

// Versión en coma flotante de inside_rounded, para el muestreo afín.
bool inside_rounded_f(float x, float y, float w, float h, float r) {
    if (r <= 0.0f) return true;
    r = std::min({r, w * 0.5f, h * 0.5f});
    float dx = 0.0f, dy = 0.0f;
    if (x < r && y < r) { dx = r - x; dy = r - y; }
    else if (x > w - r && y < r) { dx = x - (w - r); dy = r - y; }
    else if (x < r && y > h - r) { dx = r - x; dy = y - (h - r); }
    else if (x > w - r && y > h - r) { dx = x - (w - r); dy = y - (h - r); }
    else return true;
    return (dx * dx + dy * dy) <= r * r;
}

} // namespace

// -----------------------------------------------------------------------------
// Session 8: composición afín con supermuestreo 2x2
// -----------------------------------------------------------------------------
void Framebuffer::fill_affine_box(const float m[6], float rx, float ry, float rw, float rh,
                                  const std::function<uint32_t(float, float)>& shader) {
    if (rw <= 0.0f || rh <= 0.0f || !shader) return;

    // Inversa de [a c e; b d f].
    const float det = m[0] * m[3] - m[1] * m[2];
    if (std::fabs(det) < 1e-8f) return; // matriz degenerada: nada visible
    const float inv_det = 1.0f / det;
    const float ia = m[3] * inv_det;
    const float ib = -m[1] * inv_det;
    const float ic = -m[2] * inv_det;
    const float id = m[0] * inv_det;
    const float ie = (m[2] * m[5] - m[3] * m[4]) * inv_det;
    const float if_ = (m[1] * m[4] - m[0] * m[5]) * inv_det;

    // Caja envolvente en dispositivo de las cuatro esquinas transformadas.
    const float cx[4] = {rx, rx + rw, rx, rx + rw};
    const float cy[4] = {ry, ry, ry + rh, ry + rh};
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    for (int i = 0; i < 4; ++i) {
        const float dx = m[0] * cx[i] + m[2] * cy[i] + m[4];
        const float dy = m[1] * cx[i] + m[3] * cy[i] + m[5];
        min_x = std::min(min_x, dx);
        max_x = std::max(max_x, dx);
        min_y = std::min(min_y, dy);
        max_y = std::max(max_y, dy);
    }

    int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    int x1 = std::min(width_, static_cast<int>(std::ceil(max_x)) + 1);
    int y1 = std::min(height_, static_cast<int>(std::ceil(max_y)) + 1);

    // Desplazamientos de los 4 subpíxeles (rejilla ordenada 2x2).
    static const float kOff[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f},
                                     {0.25f, 0.75f}, {0.75f, 0.75f}};

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            uint32_t acc_a = 0, acc_r = 0, acc_g = 0, acc_b = 0;
            int hits = 0;
            for (const auto& off : kOff) {
                const float sx = static_cast<float>(px) + off[0];
                const float sy = static_cast<float>(py) + off[1];
                const float lx = ia * sx + ic * sy + ie;
                const float ly = ib * sx + id * sy + if_;
                if (lx < rx || lx >= rx + rw || ly < ry || ly >= ry + rh) continue;
                const uint32_t c = shader(lx - rx, ly - ry);
                const uint32_t a = (c >> 24) & 0xFFu;
                if (a == 0) continue;
                acc_a += a;
                acc_r += ((c >> 16) & 0xFFu) * a;
                acc_g += ((c >> 8) & 0xFFu) * a;
                acc_b += (c & 0xFFu) * a;
                ++hits;
            }
            if (hits == 0 || acc_a == 0) continue;
            // Media ponderada por alfa de los subpíxeles cubiertos; el alfa
            // final se reparte sobre los 4 subpíxeles para dar la cobertura.
            const uint32_t out_r = acc_r / acc_a;
            const uint32_t out_g = acc_g / acc_a;
            const uint32_t out_b = acc_b / acc_a;
            const uint32_t out_a = std::min<uint32_t>(255u, acc_a / 4u);
            blend_pixel(px, py, (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b);
        }
    }
}

void Framebuffer::fill_gradient_rounded_rect(int x, int y, int w, int h, int radius,
                                             uint32_t start_argb, uint32_t end_argb, float angle_deg) {
    if (w <= 0 || h <= 0) return;
    constexpr float kPi = 3.14159265358979323846f;
    float rad = angle_deg * (kPi / 180.0f);
    // CSS linear-gradient angle: 0deg=up, 90deg=right, 135deg=bottom-right, 180deg=down
    float vx = std::sin(rad);
    float vy = -std::cos(rad);

    float p00 = 0.0f;
    float p10 = static_cast<float>(w) * vx;
    float p01 = static_cast<float>(h) * vy;
    float p11 = p10 + p01;
    float min_p = std::min({p00, p10, p01, p11});
    float max_p = std::max({p00, p10, p01, p11});
    float inv_span = (max_p - min_p > 1e-4f) ? (1.0f / (max_p - min_p)) : 0.0f;

    float a0 = static_cast<float>((start_argb >> 24) & 0xFFu);
    float r0 = static_cast<float>((start_argb >> 16) & 0xFFu);
    float g0 = static_cast<float>((start_argb >> 8) & 0xFFu);
    float b0 = static_cast<float>(start_argb & 0xFFu);

    float a1 = static_cast<float>((end_argb >> 24) & 0xFFu);
    float r1 = static_cast<float>((end_argb >> 16) & 0xFFu);
    float g1 = static_cast<float>((end_argb >> 8) & 0xFFu);
    float b1 = static_cast<float>(end_argb & 0xFFu);

    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
    for (int py = y0; py < y1; ++py) {
        int ly = py - y;
        float py_term = (static_cast<float>(ly) + 0.5f) * vy;
        for (int px = x0; px < x1; ++px) {
            int lx = px - x;
            if (radius > 0 && !inside_rounded(lx, ly, w, h, radius)) continue;
            float proj = (static_cast<float>(lx) + 0.5f) * vx + py_term;
            float t = std::clamp((proj - min_p) * inv_span, 0.0f, 1.0f);
            uint32_t ca = static_cast<uint32_t>(std::round(a0 + (a1 - a0) * t));
            uint32_t cr = static_cast<uint32_t>(std::round(r0 + (r1 - r0) * t));
            uint32_t cg = static_cast<uint32_t>(std::round(g0 + (g1 - g0) * t));
            uint32_t cb = static_cast<uint32_t>(std::round(b0 + (b1 - b0) * t));
            blend_pixel(px, py, (ca << 24) | (cr << 16) | (cg << 8) | cb);
        }
    }
}

void Framebuffer::stroke_rounded_rect(int x, int y, int w, int h, int radius, int thickness, uint32_t argb) {
    thickness = std::max(1, thickness);
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            int lx = px - x, ly = py - y;
            if (!inside_rounded(lx, ly, w, h, radius)) continue;
            bool inside_inner = (lx >= thickness && lx < w - thickness &&
                                 ly >= thickness && ly < h - thickness &&
                                 inside_rounded(lx - thickness, ly - thickness,
                                                w - 2 * thickness, h - 2 * thickness,
                                                std::max(0, radius - thickness)));
            if (!inside_inner) blend_pixel(px, py, argb);
        }
    }
}

void Framebuffer::draw_text(int x, int y, std::string_view raw_text, uint32_t argb,
                            float font_size, bool bold, bool monospace) {
    auto& fe = FontEngine::instance();
    if (fe.freetype_active()) {
        int px = static_cast<int>(std::round(std::clamp(font_size, 8.0f, 72.0f)));
        int baseline_y = y + static_cast<int>(std::round(px * 0.88f));
        uint32_t base_a = (argb >> 24) & 0xFFu;
        uint32_t rgb = argb & 0x00FFFFFFu;
        int cx = x;
        size_t idx = 0;
        while (idx < raw_text.size()) {
            uint32_t cp = FontEngine::next_utf8_codepoint(raw_text, idx);
            const CachedGlyph* g = fe.get_glyph(cp, px, bold, monospace);
            if (!g) { cx += px / 2; continue; }
            int gx0 = cx + g->bearing_x;
            int gy0 = baseline_y - g->bearing_y;
            for (int row = 0; row < g->height; ++row) {
                int py = gy0 + row;
                if (py < 0 || py >= height_) continue;
                for (int col = 0; col < g->width; ++col) {
                    uint8_t cov = g->alpha[static_cast<size_t>(row * g->width + col)];
                    if (cov == 0) continue;
                    int pxx = gx0 + col;
                    if (pxx < 0 || pxx >= width_) continue;
                    uint32_t eff_a = (base_a * cov) / 255u;
                    if (eff_a > 0) blend_pixel(pxx, py, (eff_a << 24) | rgb);
                }
            }
            cx += g->advance_x;
            if (cx > width_) break;
        }
        return;
    }

    std::string text = utf8_to_printable(raw_text);
    int scale = (font_size >= 26.0f) ? 3 : ((font_size >= 18.0f) ? 2 : 1);
    int adv = scale * 8 + (bold ? 1 : 0);
    int cx = x;

    for (char ch : text) {
        const uint8_t* bm = glyph_bitmap(static_cast<unsigned char>(ch));
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = bm[row];
            for (int col = 0; col < 8; ++col) {
                if (bits & (1u << col)) {
                    fill_rect(cx + col * scale, y + row * scale,
                              scale + (bold ? 1 : 0), scale, argb);
                }
            }
        }
        cx += adv;
        if (cx > width_) break;
    }
}

void Framebuffer::blit_scaled(int dst_x, int dst_y, int dst_w, int dst_h,
                              const uint32_t* src, int src_w, int src_h) {
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    for (int y = 0; y < dst_h; ++y) {
        int ty = dst_y + y;
        if (ty < 0 || ty >= height_) continue;
        int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            int tx = dst_x + x;
            if (tx < 0 || tx >= width_) continue;
            int sx = (x * src_w) / dst_w;
            blend_pixel(tx, ty, src[sy * src_w + sx]);
        }
    }
}

int Framebuffer::draw_browser_chrome(const ChromeStatus& st) {
    const int tab_bar_h = 38;
    const int nav_bar_h = 46;
    const int total_h = tab_bar_h + nav_bar_h;

    fill_rect(0, 0, width_, tab_bar_h, 0xFF0A0E16u);
    fill_rect(0, tab_bar_h, width_, nav_bar_h, 0xFF111827u);
    fill_rect(0, total_h - 1, width_, 1, 0xFF1F2937u);

    // Brand button -> astra://home
    fill_rounded_rect(10, 7, 72, 24, 6, 0xFF0284C7u);
    draw_text(20, 12, "ASTRA", 0xFFFFFFFFu, 13.0f, true, false);

    // Tabs with per-tab [x] close button
    int tx = 92;
    size_t max_tabs = std::min<size_t>(st.tab_titles.size(), 5);
    size_t rendered_tabs = 0;
    for (size_t i = 0; i < max_tabs; ++i) {
        if (tx + 205 > width_ - 44) break;
        bool active = (i == st.active_tab_index);
        int tw = 205;
        fill_rounded_rect(tx, 6, tw, 28, 8, active ? 0xFF1E293Bu : 0xFF111827u);
        if (active) fill_rect(tx + 12, 6, tw - 24, 2, 0xFF38BDF8u);
        std::string lbl = st.tab_titles[i];
        {
            size_t cp = 0, cut_pos = 0;
            bool truncated = false;
            for (size_t k = 0; k < lbl.size(); ) {
                unsigned char c = static_cast<unsigned char>(lbl[k]);
                size_t clen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
                if (cp == 18) cut_pos = k;
                if (cp >= 20) { truncated = true; break; }
                k += std::min(clen, lbl.size() - k);
                ++cp;
            }
            if (truncated && cut_pos > 0) lbl = lbl.substr(0, cut_pos) + "..";
        }
        draw_text(tx + 12, 13, lbl, active ? 0xFFF8FAFCu : 0xFF94A3B8u, 12.0f, active, false);
        // Close [x] chip
        fill_rounded_rect(tx + tw - 24, 11, 18, 18, 4, active ? 0xFF334155u : 0xFF1E293Bu);
        draw_text(tx + tw - 19, 13, "x", 0xFFCBD5E1u, 11.0f, true, true);
        tx += tw + 8;
        ++rendered_tabs;
    }
    (void)rendered_tabs;

    // New Tab [+] button
    if (tx + 30 < width_) {
        fill_rounded_rect(tx, 7, 30, 26, 6, 0xFF1E293Bu);
        stroke_rounded_rect(tx, 7, 30, 26, 6, 1, 0xFF334155u);
        draw_text(tx + 10, 11, "+", 0xFF38BDF8u, 15.0f, true, true);
    }

    // Back [<], Forward [>], Reload [R] navigation buttons
    auto draw_nav_btn = [&](int bx, const char* sym, bool enabled) {
        fill_rounded_rect(bx, tab_bar_h + 8, 28, 30, 7, enabled ? 0xFF1E293Bu : 0xFF0F172Au);
        stroke_rounded_rect(bx, tab_bar_h + 8, 28, 30, 7, 1, enabled ? 0xFF334155u : 0xFF1E293Bu);
        draw_text(bx + 9, tab_bar_h + 15, sym, enabled ? 0xFFE2E8F0u : 0xFF475569u, 13.0f, true, true);
    };
    draw_nav_btn(14, "<", st.can_go_back);
    draw_nav_btn(48, ">", st.can_go_forward);
    draw_nav_btn(82, "R", true);

    // Editable Omnibox
    int bar_x = 120;
    int bar_w = std::max(240, width_ - 520);
    fill_rounded_rect(bar_x, tab_bar_h + 8, bar_w, 30, 8, st.address_bar_focused ? 0xFF0F1B2Eu : 0xFF0B1120u);
    uint32_t bar_border = st.address_bar_focused ? 0xFF38BDF8u : (st.tls_verified ? 0xFF10B981u : 0xFF334155u);
    stroke_rounded_rect(bar_x, tab_bar_h + 8, bar_w, 30, 8, st.address_bar_focused ? 2 : 1, bar_border);

    std::string url_disp = st.address_bar_focused
                               ? st.address_bar_text
                               : ((st.tls_verified ? "[TLS] " : "") + st.active_url);
    draw_text(bar_x + 12, tab_bar_h + 15, url_disp, 0xFFE2E8F0u, 13.0f, false, true);
    if (st.address_bar_focused) {
        float tw = FontEngine::instance().measure_text(url_disp, 13.0f, false, true);
        int caret_x = bar_x + 13 + static_cast<int>(std::round(tw));
        if (caret_x < bar_x + bar_w - 8) {
            fill_rect(caret_x, tab_bar_h + 14, 2, 16, 0xFF38BDF8u);
        }
    }

    int rx = bar_x + bar_w + 12;
    std::string shield_txt = "SHIELD:" + std::to_string(st.trackers_blocked);
    fill_rounded_rect(rx, tab_bar_h + 8, 108, 30, 8, 0xFF064E3Bu);
    draw_text(rx + 12, tab_bar_h + 15, shield_txt, 0xFF6EE7B7u, 12.0f, true, true);

    rx += 116;
    std::string ram_txt = "RAM:" + std::to_string(st.rss_mb) + "/" + std::to_string(st.ceiling_mb) + "MB";
    uint32_t ram_bg = (st.rss_mb * 100 / std::max<uint64_t>(1, st.ceiling_mb) > 80) ? 0xFF7F1D1Du : 0xFF1E293Bu;
    fill_rounded_rect(rx, tab_bar_h + 8, 180, 30, 8, ram_bg);
    draw_text(rx + 12, tab_bar_h + 15, ram_txt, 0xFF38BDF8u, 12.0f, true, true);

    rx += 188;
    if (rx + 76 <= width_) {
        std::string ext_txt = "EXT:" + std::to_string(st.extensions_loaded);
        fill_rounded_rect(rx, tab_bar_h + 8, 76, 30, 8, 0xFF312E81u);
        draw_text(rx + 12, tab_bar_h + 15, ext_txt, 0xFFC7D2FEu, 12.0f, true, true);
    }

    return total_h;
}

void Framebuffer::composite_display_list(const DisplayList& dl,
                                         int y_offset,
                                         float scroll_y,
                                         const VideoProvider& video_provider,
                                         const ImageProvider& image_provider,
                                         const Camera3D& cam3d,
                                         float time3d_sec,
                                         const AudioProvider& audio_provider,
                                         const CanvasProvider& canvas_provider) {
    clear_clip_rect();
    fill_rect(0, y_offset, width_, height_ - y_offset, dl.clear_color_argb);

    for (const auto& item : dl.items) {
        int ix = static_cast<int>(std::round(item.rect.x));
        float eff_y = item.fixed_viewport ? item.rect.y : (item.rect.y - scroll_y);
        int iy = static_cast<int>(std::round(eff_y)) + y_offset;
        int iw = static_cast<int>(std::round(item.rect.w));
        int ih = static_cast<int>(std::round(item.rect.h));

        if (iy >= height_ || iy + ih <= y_offset) continue;

        // Session 8: la matriz del item vive en espacio de DisplayList; el
        // compositor le compone la traslación de scroll/chrome para obtener la
        // matriz de dispositivo que consume fill_affine_box.
        const float dev_dy = item.fixed_viewport
                                 ? static_cast<float>(y_offset)
                                 : (static_cast<float>(y_offset) - scroll_y);
        float dev_m[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, dev_dy};
        if (item.has_transform) {
            dev_m[0] = item.xform[0];
            dev_m[1] = item.xform[1];
            dev_m[2] = item.xform[2];
            dev_m[3] = item.xform[3];
            dev_m[4] = item.xform[4];
            dev_m[5] = item.xform[5] + dev_dy;
        }

        if (item.has_clip) {
            int clx = static_cast<int>(std::round(item.clip_rect.x));
            float cl_eff_y = item.fixed_viewport ? item.clip_rect.y : (item.clip_rect.y - scroll_y);
            int cly = std::max(y_offset, static_cast<int>(std::round(cl_eff_y)) + y_offset);
            int clb = std::min(height_, static_cast<int>(std::round(cl_eff_y + item.clip_rect.h)) + y_offset);
            int clw = static_cast<int>(std::round(item.clip_rect.w));
            set_clip_rect(clx, cly, clw, std::max(0, clb - cly));
        } else {
            set_clip_rect(0, y_offset, width_, height_ - y_offset);
        }

        switch (item.type) {
            case DisplayItemType::DropShadow: {
                int blur = std::clamp(static_cast<int>(std::round(item.thickness)), 2, 24);
                uint32_t base_a = (item.color_argb >> 24) & 0xFFu;
                uint32_t rgb = item.color_argb & 0x00FFFFFFu;
                const float scales[3] = {0.55f, 0.30f, 0.14f};
                const int spreads[3] = {std::max(1, blur / 5), std::max(2, blur / 3), std::max(3, blur / 2)};
                for (int ring = 2; ring >= 0; --ring) {
                    int sp = spreads[ring];
                    int sx = ix - sp;
                    int sy = iy - sp;
                    int sw = iw + sp * 2;
                    int sh = ih + sp * 2;
                    int cy = std::max(y_offset, sy);
                    int ch = std::min(height_, sy + sh) - cy;
                    uint32_t ra = static_cast<uint32_t>(std::round(base_a * scales[ring]));
                    if (ch > 0 && ra > 0) {
                        fill_rounded_rect(sx, cy, sw, ch, static_cast<int>(item.radius) + sp / 2, (ra << 24) | rgb);
                    }
                }
                break;
            }
            case DisplayItemType::FillRect: {
                if (item.has_transform) {
                    const float rw = item.rect.w, rh = item.rect.h, rr = item.radius;
                    const uint32_t col = item.color_argb;
                    fill_affine_box(dev_m, item.rect.x, item.rect.y, rw, rh,
                                    [rw, rh, rr, col](float lx, float ly) -> uint32_t {
                                        return inside_rounded_f(lx, ly, rw, rh, rr) ? col : 0u;
                                    });
                    break;
                }
                int cy = std::max(y_offset, iy);
                int ch = std::min(height_, iy + ih) - cy;
                if (ch > 0) fill_rounded_rect(ix, cy, iw, ch, static_cast<int>(item.radius), item.color_argb);
                break;
            }
            case DisplayItemType::FillGradient: {
                if (item.has_transform) {
                    const float rw = item.rect.w, rh = item.rect.h, rr = item.radius;
                    const uint32_t c0 = item.color_argb, c1 = item.color2_argb;
                    const float ang = item.angle_deg;
                    fill_affine_box(dev_m, item.rect.x, item.rect.y, rw, rh,
                                    [rw, rh, rr, c0, c1, ang](float lx, float ly) -> uint32_t {
                                        if (!inside_rounded_f(lx, ly, rw, rh, rr)) return 0u;
                                        return gradient_color_at(lx, ly, rw, rh, c0, c1, ang);
                                    });
                    break;
                }
                if (iw > 0 && ih > 0) {
                    fill_gradient_rounded_rect(ix, iy, iw, ih, static_cast<int>(item.radius),
                                               item.color_argb, item.color2_argb, item.angle_deg);
                }
                break;
            }
            case DisplayItemType::StrokeRect: {
                if (item.has_transform) {
                    const float rw = item.rect.w, rh = item.rect.h, rr = item.radius;
                    const float th = std::max(1.0f, item.thickness);
                    const uint32_t col = item.color_argb;
                    fill_affine_box(dev_m, item.rect.x, item.rect.y, rw, rh,
                                    [rw, rh, rr, th, col](float lx, float ly) -> uint32_t {
                                        if (!inside_rounded_f(lx, ly, rw, rh, rr)) return 0u;
                                        const bool inner =
                                            lx >= th && lx < rw - th && ly >= th && ly < rh - th &&
                                            inside_rounded_f(lx - th, ly - th, rw - 2 * th, rh - 2 * th,
                                                             std::max(0.0f, rr - th));
                                        return inner ? 0u : col;
                                    });
                    break;
                }
                int cy = std::max(y_offset, iy);
                int ch = std::min(height_, iy + ih) - cy;
                if (ch > 0) stroke_rounded_rect(ix, cy, iw, ch, static_cast<int>(item.radius),
                                                static_cast<int>(item.thickness), item.color_argb);
                break;
            }
            case DisplayItemType::DrawText:
                if (item.has_transform) {
                    // El trazado de glifos rotados exigiría rasterizar cada
                    // contorno bajo la matriz; el compositor software aplica la
                    // parte de traslación y escala (la que domina en las
                    // animaciones reales) y mantiene los glifos sin cizalla.
                    const float ox = item.rect.x;
                    const float oy = item.rect.y;
                    const float tx = dev_m[0] * ox + dev_m[2] * oy + dev_m[4];
                    const float ty = dev_m[1] * ox + dev_m[3] * oy + dev_m[5];
                    const float sx = std::sqrt(dev_m[0] * dev_m[0] + dev_m[1] * dev_m[1]);
                    const float sy = std::sqrt(dev_m[2] * dev_m[2] + dev_m[3] * dev_m[3]);
                    const float scale = std::max(0.1f, 0.5f * (sx + sy));
                    const int txi = static_cast<int>(std::round(tx));
                    const int tyi = static_cast<int>(std::round(ty));
                    if (tyi >= y_offset && tyi < height_) {
                        draw_text(txi, tyi, item.text, item.color_argb,
                                  item.font_size * scale, item.bold, item.monospace);
                    }
                    break;
                }
                if (iy >= y_offset) {
                    draw_text(ix, iy, item.text, item.color_argb, item.font_size, item.bold, item.monospace);
                }
                break;
            case DisplayItemType::DrawInput: {
                int cy = std::max(y_offset, iy);
                int ch = std::min(height_, iy + ih) - cy;
                if (ch > 0) {
                    int rad = static_cast<int>(item.radius);
                    fill_rounded_rect(ix, cy, iw, ch, rad, item.focused ? 0xFF0F1B2Eu : 0xFF0B1120u);
                    stroke_rounded_rect(ix, cy, iw, ch, rad, item.focused ? 2 : 1,
                                        item.focused ? 0xFF38BDF8u : 0xFF334155u);
                    int ty = iy + std::max(4, (ih - static_cast<int>(item.font_size)) / 2 - 1);
                    if (ty >= y_offset && ty < height_ - 4) {
                        draw_text(ix + 12, ty, item.text, item.color_argb, item.font_size, false, false);
                        if (item.focused) {
                            float tw = (item.color_argb == 0xFF64748Bu)
                                           ? 0.0f
                                           : FontEngine::instance().measure_text(item.text, item.font_size, false, false);
                            int caret_x = ix + 13 + static_cast<int>(std::round(tw));
                            if (caret_x < ix + iw - 6) {
                                fill_rect(caret_x, ty, 2, static_cast<int>(item.font_size) + 2, 0xFF38BDF8u);
                            }
                        }
                    }
                }
                break;
            }
            case DisplayItemType::DrawImage: {
                int cx = std::max(0, ix);
                int cy = std::max(y_offset, iy);
                int cw = std::min(width_ - cx, iw);
                int ch = std::min(height_ - cy, ih);
                DecodedImage img;
                if (image_provider && image_provider(item.text, img) && img.valid) {
                    blit_scaled(cx, cy, cw, ch, img.argb.data(), img.width, img.height);
                    stroke_rounded_rect(cx, cy, cw, ch, 6, 1, 0xFF334155u);
                } else {
                    fill_rounded_rect(cx, cy, cw, ch, 6, 0xFF1E293Bu);
                    draw_text(cx + 8, cy + ch / 2 - 6, "[IMG] " + item.text, 0xFF94A3B8u, 11.0f, false, true);
                }
                break;
            }
            case DisplayItemType::Embed3D: {
                int cx = std::max(0, ix);
                int cy = std::max(y_offset, iy);
                int cw = std::min(width_ - cx, iw);
                int ch = std::min(height_ - cy, ih);
                if (cw > 16 && ch > 16) {
                    auto st3d = Astra3D::render_scene_into(pixels_.data(), width_, cx, cy, cw, ch,
                                                           item.text, time3d_sec, cam3d);
                    stroke_rounded_rect(cx, cy, cw, ch, 6, 1, 0xFF38BDF8u);
                    std::string hud = "Astra3D Smooth | " + std::to_string(st3d.triangles_rasterized) + " tris";
                    int hud_w = std::min(cw - 20, static_cast<int>(FontEngine::instance().measure_text(hud, 11.0f, true, true)) + 20);
                    fill_rounded_rect(cx + 10, cy + ch - 28, hud_w, 22, 4, 0xCC0A0F1Au);
                    draw_text(cx + 18, cy + ch - 23, hud, 0xFF7DD3FCu, 11.0f, true, true);
                }
                break;
            }
            case DisplayItemType::EmbedVideo: {
                int cx = std::max(0, ix);
                int cy = std::max(y_offset, iy);
                int cw = std::min(width_ - cx, iw);
                int ch = std::min(height_ - cy, ih);
                VideoBlitFrame frame;
                if (video_provider && video_provider(item.text, frame) &&
                    frame.width > 0 && frame.height > 0 && !frame.argb.empty()) {
                    blit_scaled(cx, cy, cw, ch, frame.argb.data(), frame.width, frame.height);
                    stroke_rounded_rect(cx, cy, cw, ch, 6, 1, 0xFF34D399u);
                    // Interactive transport bar at bottom of video frame
                    if (ch >= 48 && cw >= 160) {
                        int bar_h = 34;
                        int by = cy + ch - bar_h - 4;
                        fill_rounded_rect(cx + 6, by, cw - 12, bar_h, 6, 0xD806141Eu);
                        // Play indicator button
                        fill_rounded_rect(cx + 12, by + 6, 24, 22, 4, 0xFF10B981u);
                        draw_text(cx + 19, by + 10, ">", 0xFF042F2Eu, 12.0f, true, true);
                        // Scrubber bar
                        int track_x = cx + 44;
                        int track_w = std::max(40, cw - 60);
                        int track_y = by + 7;
                        fill_rounded_rect(track_x, track_y, track_w, 6, 3, 0xFF1E293Bu);
                        float prog = std::clamp(frame.progress, 0.0f, 1.0f);
                        int fill_w = std::max(4, static_cast<int>(track_w * prog));
                        fill_rounded_rect(track_x, track_y, fill_w, 6, 3, 0xFF34D399u);
                        fill_rounded_rect(track_x + fill_w - 4, track_y - 2, 8, 10, 4, 0xFFECFDF5u);
                        // Time & codec label below scrubber
                        std::string hud = frame.time_label.empty() ? frame.label : (frame.time_label + " | " + frame.label);
                        draw_text(track_x, by + 16, hud, 0xFF6EE7B7u, 10.5f, true, true);
                    }
                } else {
                    fill_rounded_rect(cx, cy, cw, ch, 8, 0xFF0F172Au);
                    stroke_rounded_rect(cx, cy, cw, ch, 8, 1, 0xFF475569u);
                    draw_text(cx + 16, cy + ch / 2 - 6, "AstraMedia Video: " + item.text, 0xFF94A3B8u, 13.0f, true, false);
                }
                break;
            }
            case DisplayItemType::EmbedAudio: {
                int cx = std::max(0, ix);
                int cy = std::max(y_offset, iy);
                int cw = std::min(width_ - cx, iw);
                int ch = std::min(height_ - cy, ih);
                if (cw > 60 && ch > 44) {
                    fill_rounded_rect(cx, cy, cw, ch, 10, 0xFF0B1324u);
                    stroke_rounded_rect(cx, cy, cw, ch, 10, 1, 0xFF38BDF8u);

                    AudioBlitCard card;
                    bool has_audio = audio_provider && audio_provider(item.text, card) && card.valid;
                    std::string title = has_audio ? card.label : ("AstraAudio: " + item.text);
                    std::string meta = has_audio ? card.meta : "PCM s16le | ALSA Ready";

                    // Top band: title; Bottom band: metadata
                    draw_text(cx + 16, cy + 10, title, 0xFFE2E8F0u, 12.0f, true, true);
                    draw_text(cx + 16, cy + ch - 20, meta, 0xFF7DD3FCu, 10.5f, false, true);

                    // Middle band: Play button + PCM waveform bars (zero overlap with header/footer)
                    int mid_top = cy + 30;
                    int mid_bot = cy + ch - 26;
                    int mid_h = std::max(14, mid_bot - mid_top);
                    int mid_center = mid_top + mid_h / 2;

                    int btn_sz = std::min(34, mid_h);
                    fill_rounded_rect(cx + 16, mid_center - btn_sz / 2, btn_sz, btn_sz, 8, 0xFF0284C7u);
                    draw_text(cx + 16 + btn_sz / 2 - 4, mid_center - 7, ">", 0xFFFFFFFFu, 13.0f, true, true);

                    int wave_x = cx + 16 + btn_sz + 14;
                    int wave_w = std::max(40, (cx + cw - 16) - wave_x);
                    int wave_h = std::max(12, mid_h - 6);
                    const std::vector<float>& peaks = card.waveform_peaks;
                    size_t nb = std::min<size_t>(peaks.empty() ? 32 : peaks.size(),
                                                 static_cast<size_t>(std::max(12, wave_w / 7)));
                    int step_x = std::max(4, wave_w / static_cast<int>(std::max<size_t>(1, nb)));
                    int bar_w = std::max(2, step_x - 3);
                    for (size_t b = 0; b < nb; ++b) {
                        float raw = peaks.empty() ? (0.35f + 0.45f * std::sin(static_cast<float>(b) * 0.55f))
                                                  : peaks[(b * peaks.size()) / nb];
                        float env = 0.72f + 0.28f * std::sin(static_cast<float>(b) * 0.42f);
                        float amp = std::clamp(raw * env, 0.16f, 1.0f);
                        int bh = std::max(4, static_cast<int>(amp * static_cast<float>(wave_h)));
                        int bx = wave_x + static_cast<int>(b) * step_x;
                        uint32_t col = (b % 3 == 0) ? 0xFF38BDF8u : 0xFF34D399u;
                        fill_rounded_rect(bx, mid_center - bh / 2, bar_w, bh, 2, col);
                    }
                }
                break;
            }
            case DisplayItemType::EmbedCanvas: {
                int cx = std::max(0, ix);
                int cy = std::max(y_offset, iy);
                int cw = std::min(width_ - cx, iw);
                int ch = std::min(height_ - cy, ih);
                CanvasBlitFrame cframe;
                if (canvas_provider && canvas_provider(item.text, cframe) &&
                    cframe.width > 0 && cframe.height > 0 && cframe.argb != nullptr) {
                    blit_scaled(cx, cy, cw, ch, cframe.argb, cframe.width, cframe.height);
                    stroke_rounded_rect(cx, cy, cw, ch, 6, 1, 0xFF334155u);
                } else {
                    fill_rounded_rect(cx, cy, cw, ch, 6, 0xFF0B1324u);
                    stroke_rounded_rect(cx, cy, cw, ch, 6, 1, 0xFF334155u);
                    draw_text(cx + 12, cy + ch / 2 - 6, "[CANVAS] #" + item.text, 0xFF64748Bu, 12.0f, false, true);
                }
                break;
            }
        }
    }
    clear_clip_rect();

    // Draw APZ-style scrollbar thumb if content overflows viewport
    float view_h = static_cast<float>(height_ - y_offset);
    if (dl.content_height > view_h + 4.0f) {
        int track_h = height_ - y_offset - 8;
        int thumb_h = std::max(28, static_cast<int>(track_h * (view_h / dl.content_height)));
        float max_scroll = std::max(1.0f, dl.content_height - view_h);
        float frac = std::clamp(scroll_y / max_scroll, 0.0f, 1.0f);
        int thumb_y = y_offset + 4 + static_cast<int>((track_h - thumb_h) * frac);
        fill_rounded_rect(width_ - 10, y_offset + 4, 6, track_h, 3, 0xFF1E293Bu);
        fill_rounded_rect(width_ - 10, thumb_y, 6, thumb_h, 3, 0xFF38BDF8u);
    }
}

bool Framebuffer::save_png(const std::string& path) const {
    const size_t row_bytes = static_cast<size_t>(width_) * 4 + 1;
    std::vector<uint8_t> raw;
    raw.reserve(row_bytes * static_cast<size_t>(height_));
    for (int y = 0; y < height_; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width_; ++x) {
            uint32_t p = pixels_[static_cast<size_t>(y * width_ + x)];
            raw.push_back(static_cast<uint8_t>((p >> 16) & 0xFF));
            raw.push_back(static_cast<uint8_t>((p >> 8) & 0xFF));
            raw.push_back(static_cast<uint8_t>(p & 0xFF));
            raw.push_back(static_cast<uint8_t>((p >> 24) & 0xFF));
        }
    }

    std::vector<uint8_t> zstream;
    zstream.reserve(raw.size() + (raw.size() / 65535 + 2) * 5 + 6);
    zstream.push_back(0x78);
    zstream.push_back(0x01);

    size_t offset = 0;
    while (offset < raw.size()) {
        size_t chunk = std::min<size_t>(65535, raw.size() - offset);
        bool final_block = (offset + chunk >= raw.size());
        zstream.push_back(final_block ? 0x01 : 0x00);
        uint16_t len = static_cast<uint16_t>(chunk);
        uint16_t nlen = static_cast<uint16_t>(~len);
        zstream.push_back(static_cast<uint8_t>(len & 0xFF));
        zstream.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        zstream.push_back(static_cast<uint8_t>(nlen & 0xFF));
        zstream.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        zstream.insert(zstream.end(), raw.begin() + static_cast<ptrdiff_t>(offset),
                       raw.begin() + static_cast<ptrdiff_t>(offset + chunk));
        offset += chunk;
    }
    write_be32(zstream, adler32_compute(raw.data(), raw.size()));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    write_be32(ihdr, static_cast<uint32_t>(width_));
    write_be32(ihdr, static_cast<uint32_t>(height_));
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    write_png_chunk(png, "IHDR", ihdr);
    write_png_chunk(png, "IDAT", zstream);
    write_png_chunk(png, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return f.good();
}

bool Framebuffer::present_x11_interactive(const std::string& window_title,
                                          const InteractiveCallbacks& callbacks) {
    if (!std::getenv("DISPLAY")) return false;
    void* x11 = nullptr;
    for (const char* name : platform::libx11_names()) {
        x11 = dlopen(name, RTLD_LAZY);
        if (x11) break;
    }
    if (!x11) return false;

    struct XButtonEv {
        int type; unsigned long serial; int send_event; void* display;
        unsigned long window, root, subwindow, time;
        int x, y, x_root, y_root;
        unsigned int state, button;
    };
    struct XKeyEv {
        int type; unsigned long serial; int send_event; void* display;
        unsigned long window, root, subwindow, time;
        int x, y, x_root, y_root;
        unsigned int state, keycode;
    };
    union XEv {
        int type;
        XButtonEv xbutton;
        XKeyEv xkey;
        long pad[24];
    };

    auto XOpenDisplay = reinterpret_cast<void* (*)(const char*)>(dlsym(x11, "XOpenDisplay"));
    auto XDefaultScreen = reinterpret_cast<int (*)(void*)>(dlsym(x11, "XDefaultScreen"));
    auto XRootWindow = reinterpret_cast<unsigned long (*)(void*, int)>(dlsym(x11, "XRootWindow"));
    auto XDefaultVisual = reinterpret_cast<void* (*)(void*, int)>(dlsym(x11, "XDefaultVisual"));
    auto XDefaultDepth = reinterpret_cast<int (*)(void*, int)>(dlsym(x11, "XDefaultDepth"));
    auto XCreateSimpleWindow = reinterpret_cast<unsigned long (*)(void*, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long)>(dlsym(x11, "XCreateSimpleWindow"));
    auto XStoreName = reinterpret_cast<int (*)(void*, unsigned long, const char*)>(dlsym(x11, "XStoreName"));
    auto XSelectInput = reinterpret_cast<int (*)(void*, unsigned long, long)>(dlsym(x11, "XSelectInput"));
    auto XMapWindow = reinterpret_cast<int (*)(void*, unsigned long)>(dlsym(x11, "XMapWindow"));
    auto XCreateGC = reinterpret_cast<void* (*)(void*, unsigned long, unsigned long, void*)>(dlsym(x11, "XCreateGC"));
    auto XCreateImage = reinterpret_cast<void* (*)(void*, void*, unsigned, int, int, char*, unsigned, unsigned, int, int)>(dlsym(x11, "XCreateImage"));
    auto XPutImage = reinterpret_cast<int (*)(void*, unsigned long, void*, void*, int, int, int, int, unsigned, unsigned)>(dlsym(x11, "XPutImage"));
    auto XNextEvent = reinterpret_cast<int (*)(void*, XEv*)>(dlsym(x11, "XNextEvent"));
    auto XPending = reinterpret_cast<int (*)(void*)>(dlsym(x11, "XPending"));
    auto XFlush = reinterpret_cast<int (*)(void*)>(dlsym(x11, "XFlush"));
    auto XCloseDisplay = reinterpret_cast<int (*)(void*)>(dlsym(x11, "XCloseDisplay"));

    if (!XOpenDisplay || !XCreateSimpleWindow || !XCreateImage || !XPutImage || !XNextEvent) return false;
    void* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;

    int scr = XDefaultScreen(dpy);
    unsigned long root = XRootWindow(dpy, scr);
    unsigned long win = XCreateSimpleWindow(dpy, root, 40, 40,
                                            static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                                            0, 0, 0x0D1118);
    if (XStoreName) XStoreName(dpy, win, window_title.c_str());
    // ExposureMask (1<<15) | KeyPressMask (1<<0) | ButtonPressMask (1<<2)
    if (XSelectInput) XSelectInput(dpy, win, (1L << 15) | (1L << 0) | (1L << 2));
    XMapWindow(dpy, win);
    void* gc = XCreateGC(dpy, win, 0, nullptr);

    auto blit_now = [&]() {
        std::vector<uint32_t> copy = pixels_;
        void* img = XCreateImage(dpy, XDefaultVisual(dpy, scr),
                                 static_cast<unsigned>(XDefaultDepth(dpy, scr)),
                                 2, 0, reinterpret_cast<char*>(copy.data()),
                                 static_cast<unsigned>(width_), static_cast<unsigned>(height_), 32, 0);
        if (img) {
            XPutImage(dpy, win, gc, img, 0, 0, 0, 0,
                      static_cast<unsigned>(width_), static_cast<unsigned>(height_));
            XFlush(dpy);
        }
    };

    blit_now();
    bool running = true;
    while (running) {
        if (callbacks.on_tick && XPending && XPending(dpy) == 0) {
            callbacks.on_tick();
            if (callbacks.on_repaint) callbacks.on_repaint(*this);
            blit_now();
            usleep(16000);
            continue;
        }
        XEv ev{};
        XNextEvent(dpy, &ev);
        if (ev.type == 12) { // Expose
            blit_now();
        } else if (ev.type == 4) { // ButtonPress
            if (ev.xbutton.button == 4 && callbacks.on_scroll) {
                callbacks.on_scroll(-60);
            } else if (ev.xbutton.button == 5 && callbacks.on_scroll) {
                callbacks.on_scroll(60);
            } else if (ev.xbutton.button == 1 && callbacks.on_click) {
                callbacks.on_click(ev.xbutton.x, ev.xbutton.y);
            }
            if (callbacks.on_repaint) callbacks.on_repaint(*this);
            blit_now();
        } else if (ev.type == 2) { // KeyPress (Escape=9, Backspace=22, Enter=36, Down=116, Up=111)
            using XLookupString_fn = int (*)(void*, char*, int, unsigned long*, void*);
            static auto XLookupString = reinterpret_cast<XLookupString_fn>(dlsym(x11, "XLookupString"));
            if (ev.xkey.keycode == 9) {
                running = false;
            } else if (ev.xkey.keycode == 116 && callbacks.on_scroll) {
                callbacks.on_scroll(80);
                if (callbacks.on_repaint) callbacks.on_repaint(*this);
                blit_now();
            } else if (ev.xkey.keycode == 111 && callbacks.on_scroll) {
                callbacks.on_scroll(-80);
                if (callbacks.on_repaint) callbacks.on_repaint(*this);
                blit_now();
            } else if (ev.xkey.keycode == 22 && callbacks.on_backspace) {
                callbacks.on_backspace();
                if (callbacks.on_repaint) callbacks.on_repaint(*this);
                blit_now();
            } else if (ev.xkey.keycode == 36 && callbacks.on_enter) {
                callbacks.on_enter();
                if (callbacks.on_repaint) callbacks.on_repaint(*this);
                blit_now();
            } else if (XLookupString && callbacks.on_text) {
                char buf[16] = {};
                unsigned long ks = 0;
                int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &ks, nullptr);
                if (n > 0 && buf[0] >= 32 && buf[0] < 127) {
                    callbacks.on_text(std::string(buf, static_cast<size_t>(n)));
                    if (callbacks.on_repaint) callbacks.on_repaint(*this);
                    blit_now();
                }
            }
        }
    }

    XCloseDisplay(dpy);
    return true;
}

} // namespace astra
