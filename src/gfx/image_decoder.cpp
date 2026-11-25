#include "gfx/image_decoder.hpp"

#include "core/platform.hpp"
#include "gfx/svg_engine.hpp"
#include "net/http_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <sstream>

namespace astra {

namespace {

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint8_t paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

} // namespace

DecodedImage ImageDecoder::decode_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        DecodedImage img;
        img.error = "Cannot open image file: " + path;
        return img;
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    std::string bytes = oss.str();
    return decode_bytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

DecodedImage ImageDecoder::decode_bytes(const uint8_t* data, size_t len) {
    if (!data || len < 8) {
        DecodedImage img;
        img.error = "Image buffer too small";
        return img;
    }
    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(data, kPngSig, 8) == 0) {
        return decode_png(data, len);
    }
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return decode_jpeg(data, len);
    }
    if (len >= 13 && (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0)) {
        return decode_gif(data, len);
    }
    if (len >= 16 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0) {
        return decode_webp(data, len);
    }
    if (data[0] == 'B' && data[1] == 'M' && len >= 54) {
        return decode_bmp(data, len);
    }
    if (data[0] == 'P' && data[1] == '6') {
        return decode_ppm(data, len);
    }
    // AstraImage v4: Native SVG-in-<img> vector decoding
    size_t skip = 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) skip = 3; // UTF-8 BOM
    while (skip < len && (data[skip] == ' ' || data[skip] == '\t' || data[skip] == '\r' || data[skip] == '\n')) ++skip;
    if (skip < len && data[skip] == '<') {
        std::string_view sv(reinterpret_cast<const char*>(data + skip), len - skip);
        std::string_view head = sv.substr(0, std::min<size_t>(sv.size(), 512));
        if (head.find("<svg") != std::string_view::npos || head.find("<SVG") != std::string_view::npos) {
            SvgSurface surf = SvgEngine::render_svg(sv, 0, 0);
            if (surf.valid && surf.width > 0 && surf.height > 0) {
                DecodedImage img;
                img.valid = true;
                img.format = "SVG";
                img.width = surf.width;
                img.height = surf.height;
                img.argb = std::move(surf.argb);
                return img;
            }
        }
    }
    DecodedImage img;
    img.error = "Unsupported image signature";
    return img;
}

DecodedImage ImageDecoder::decode_png(const uint8_t* data, size_t len) {
    DecodedImage out;
    out.format = "PNG";

    uint32_t w = 0, h = 0;
    uint8_t bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<uint8_t> idat;
    std::vector<uint8_t> palette;

    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t chunk_len = read_be32(data + pos);
        const char* type = reinterpret_cast<const char*>(data + pos + 4);
        pos += 8;
        if (pos + chunk_len + 4 > len) break;

        if (std::memcmp(type, "IHDR", 4) == 0 && chunk_len >= 13) {
            w = read_be32(data + pos);
            h = read_be32(data + pos + 4);
            bit_depth = data[pos + 8];
            color_type = data[pos + 9];
            interlace = data[pos + 12];
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(data + pos, data + pos + chunk_len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data + pos, data + pos + chunk_len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += chunk_len + 4; // skip CRC
    }

    if (w == 0 || h == 0 || w > 8192 || h > 8192 || bit_depth != 8 || interlace != 0) {
        out.error = "Unsupported PNG dimensions/depth/interlace";
        return out;
    }

    int bpp = 0;
    switch (color_type) {
        case 0: bpp = 1; break; // Grayscale
        case 2: bpp = 3; break; // RGB
        case 3: bpp = 1; break; // Indexed
        case 4: bpp = 2; break; // Grayscale+Alpha
        case 6: bpp = 4; break; // RGBA
        default: out.error = "Unsupported PNG color type"; return out;
    }

    std::string inflated;
    if (!HttpClient::decompress_gzip_or_zlib(idat.data(), idat.size(), inflated)) {
        out.error = "PNG IDAT zlib decompression failed";
        return out;
    }

    size_t stride = static_cast<size_t>(w) * static_cast<size_t>(bpp);
    if (inflated.size() < static_cast<size_t>(h) * (stride + 1)) {
        out.error = "PNG scanline buffer truncated";
        return out;
    }

    std::vector<uint8_t> recon(static_cast<size_t>(h) * stride, 0);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(inflated.data());

    for (uint32_t y = 0; y < h; ++y) {
        uint8_t filter = src[y * (stride + 1)];
        const uint8_t* row_in = src + y * (stride + 1) + 1;
        uint8_t* row_out = recon.data() + y * stride;
        const uint8_t* prev_row = (y > 0) ? (recon.data() + (y - 1) * stride) : nullptr;

        for (size_t i = 0; i < stride; ++i) {
            uint8_t raw = row_in[i];
            uint8_t a = (i >= static_cast<size_t>(bpp)) ? row_out[i - bpp] : 0;
            uint8_t b = prev_row ? prev_row[i] : 0;
            uint8_t c = (prev_row && i >= static_cast<size_t>(bpp)) ? prev_row[i - bpp] : 0;

            uint8_t val = raw;
            switch (filter) {
                case 0: val = raw; break;
                case 1: val = static_cast<uint8_t>(raw + a); break;
                case 2: val = static_cast<uint8_t>(raw + b); break;
                case 3: val = static_cast<uint8_t>(raw + ((static_cast<int>(a) + static_cast<int>(b)) >> 1)); break;
                case 4: val = static_cast<uint8_t>(raw + paeth_predictor(a, b, c)); break;
                default: break;
            }
            row_out[i] = val;
        }
    }

    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.argb.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = recon.data() + y * stride;
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (color_type == 6) {
                r = row[x * 4 + 0]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3];
            } else if (color_type == 2) {
                r = row[x * 3 + 0]; g = row[x * 3 + 1]; b = row[x * 3 + 2];
            } else if (color_type == 0) {
                r = g = b = row[x];
            } else if (color_type == 4) {
                r = g = b = row[x * 2]; a = row[x * 2 + 1];
            } else if (color_type == 3) {
                size_t idx = static_cast<size_t>(row[x]) * 3;
                if (idx + 2 < palette.size()) {
                    r = palette[idx]; g = palette[idx + 1]; b = palette[idx + 2];
                }
            }
            out.argb[static_cast<size_t>(y * w + x)] =
                (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
    }

    out.valid = true;
    return out;
}

DecodedImage ImageDecoder::decode_bmp(const uint8_t* data, size_t len) {
    DecodedImage out;
    out.format = "BMP";
    uint32_t pixel_off = read_le32(data + 10);
    int32_t w = static_cast<int32_t>(read_le32(data + 18));
    int32_t h_signed = static_cast<int32_t>(read_le32(data + 22));
    uint16_t bpp = read_le16(data + 28);
    bool top_down = (h_signed < 0);
    int h = std::abs(h_signed);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (bpp != 24 && bpp != 32)) {
        out.error = "Unsupported BMP format";
        return out;
    }

    size_t row_stride = ((static_cast<size_t>(w) * (bpp / 8) + 3) & ~size_t(3));
    if (pixel_off + row_stride * static_cast<size_t>(h) > len) {
        out.error = "Truncated BMP pixel data";
        return out;
    }

    out.width = w;
    out.height = h;
    out.argb.resize(static_cast<size_t>(w * h));
    for (int y = 0; y < h; ++y) {
        int src_y = top_down ? y : (h - 1 - y);
        const uint8_t* row = data + pixel_off + static_cast<size_t>(src_y) * row_stride;
        for (int x = 0; x < w; ++x) {
            uint8_t b = row[x * (bpp / 8) + 0];
            uint8_t g = row[x * (bpp / 8) + 1];
            uint8_t r = row[x * (bpp / 8) + 2];
            uint8_t a = (bpp == 32) ? row[x * 4 + 3] : 255;
            out.argb[static_cast<size_t>(y * w + x)] =
                (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
    }
    out.valid = true;
    return out;
}

DecodedImage ImageDecoder::decode_ppm(const uint8_t* data, size_t len) {
    DecodedImage out;
    out.format = "PPM";
    size_t pos = 2;
    auto next_int = [&]() -> int {
        while (pos < len) {
            if (data[pos] == '#') {
                while (pos < len && data[pos] != '\n') ++pos;
            } else if (std::isspace(data[pos])) {
                ++pos;
            } else {
                break;
            }
        }
        int val = 0;
        while (pos < len && std::isdigit(data[pos])) {
            val = val * 10 + (data[pos++] - '0');
        }
        return val;
    };
    int w = next_int(), h = next_int(), maxv = next_int();
    if (pos < len && std::isspace(data[pos])) ++pos;
    if (w <= 0 || h <= 0 || maxv != 255 || pos + static_cast<size_t>(w * h * 3) > len) {
        out.error = "Invalid PPM P6 image";
        return out;
    }
    out.width = w;
    out.height = h;
    out.argb.resize(static_cast<size_t>(w * h));
    for (int i = 0; i < w * h; ++i) {
        uint8_t r = data[pos + i * 3 + 0];
        uint8_t g = data[pos + i * 3 + 1];
        uint8_t b = data[pos + i * 3 + 2];
        out.argb[static_cast<size_t>(i)] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    out.valid = true;
    return out;
}

DecodedImage ImageDecoder::decode_jpeg(const uint8_t* data, size_t len) {
    DecodedImage out;
    out.format = "JPEG";

    static constexpr int kZigZag[64] = {
         0,  1,  8, 16,  9,  2,  3, 10,
        17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63
    };

    struct HuffTable {
        bool valid = false;
        int counts[16] = {};
        std::vector<uint8_t> symbols;
        std::vector<uint32_t> codes;
        std::vector<uint8_t> lengths;
    };

    struct JpegComp {
        uint8_t id = 0;
        int h_samp = 1;
        int v_samp = 1;
        int qt_id = 0;
        int dc_id = 0;
        int ac_id = 0;
        int dc_pred = 0;
    };

    uint16_t qt[4][64] = {};
    bool qt_valid[4] = {};
    HuffTable huff_dc[4];
    HuffTable huff_ac[4];
    JpegComp comps[4];
    int num_comps = 0;
    int width = 0, height = 0;
    int max_h = 1, max_v = 1;
    int restart_interval = 0;
    size_t scan_offset = 0;

    auto read_be16 = [](const uint8_t* p) -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    };

    size_t pos = 2;
    while (pos + 2 <= len) {
        if (data[pos] != 0xFF) { ++pos; continue; }
        while (pos < len && data[pos] == 0xFF) ++pos;
        if (pos >= len) break;
        uint8_t marker = data[pos++];
        if (marker == 0xD9) break; // EOI
        if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;

        if (pos + 2 > len) break;
        uint16_t seg_len = read_be16(data + pos);
        if (seg_len < 2 || pos + seg_len > len) break;
        const uint8_t* seg = data + pos + 2;
        size_t slen = seg_len - 2;

        if (marker == 0xDB) { // DQT
            size_t o = 0;
            while (o < slen) {
                uint8_t pq_tq = seg[o++];
                int prec = pq_tq >> 4;
                int tq = pq_tq & 0x0F;
                if (tq > 3) break;
                if (prec == 0) {
                    if (o + 64 > slen) break;
                    for (int i = 0; i < 64; ++i) qt[tq][kZigZag[i]] = seg[o++];
                } else {
                    if (o + 128 > slen) break;
                    for (int i = 0; i < 64; ++i) {
                        qt[tq][kZigZag[i]] = read_be16(seg + o);
                        o += 2;
                    }
                }
                qt_valid[tq] = true;
            }
        } else if (marker == 0xC0) { // SOF0 Baseline DCT
            if (slen < 6) { out.error = "Truncated SOF0"; return out; }
            uint8_t precision = seg[0];
            height = read_be16(seg + 1);
            width = read_be16(seg + 3);
            num_comps = seg[5];
            if (precision != 8 || (num_comps != 1 && num_comps != 3) || slen < static_cast<size_t>(6 + num_comps * 3)) {
                out.error = "Unsupported JPEG SOF0 format";
                return out;
            }
            max_h = 1;
            max_v = 1;
            for (int c = 0; c < num_comps; ++c) {
                comps[c].id = seg[6 + c * 3];
                uint8_t hv = seg[6 + c * 3 + 1];
                comps[c].h_samp = std::max(1, static_cast<int>(hv >> 4));
                comps[c].v_samp = std::max(1, static_cast<int>(hv & 0x0F));
                comps[c].qt_id = seg[6 + c * 3 + 2] & 3;
                max_h = std::max(max_h, comps[c].h_samp);
                max_v = std::max(max_v, comps[c].v_samp);
            }
        } else if (marker == 0xC2) {
            out.error = "Progressive JPEG (SOF2) not yet supported";
            return out;
        } else if (marker == 0xC4) { // DHT
            size_t o = 0;
            while (o + 17 <= slen) {
                uint8_t tc_th = seg[o++];
                int tc = tc_th >> 4;
                int th = tc_th & 0x0F;
                if (th > 3) break;
                HuffTable& ht = (tc == 0) ? huff_dc[th] : huff_ac[th];
                ht = HuffTable{};
                int total_syms = 0;
                for (int i = 0; i < 16; ++i) {
                    ht.counts[i] = seg[o++];
                    total_syms += ht.counts[i];
                }
                if (o + static_cast<size_t>(total_syms) > slen) break;
                ht.symbols.assign(seg + o, seg + o + total_syms);
                o += static_cast<size_t>(total_syms);
                uint32_t code = 0;
                for (int bits = 1; bits <= 16; ++bits) {
                    for (int k = 0; k < ht.counts[bits - 1]; ++k) {
                        ht.codes.push_back(code++);
                        ht.lengths.push_back(static_cast<uint8_t>(bits));
                    }
                    code <<= 1;
                }
                ht.valid = true;
            }
        } else if (marker == 0xDD) { // DRI
            if (slen >= 2) restart_interval = read_be16(seg);
        } else if (marker == 0xDA) { // SOS
            if (slen < 1) { out.error = "Invalid SOS"; return out; }
            int ns = seg[0];
            if (slen < static_cast<size_t>(1 + ns * 2 + 3)) { out.error = "Truncated SOS"; return out; }
            for (int i = 0; i < ns; ++i) {
                uint8_t cid = seg[1 + i * 2];
                uint8_t td_ta = seg[1 + i * 2 + 1];
                for (int c = 0; c < num_comps; ++c) {
                    if (comps[c].id == cid) {
                        comps[c].dc_id = (td_ta >> 4) & 3;
                        comps[c].ac_id = td_ta & 3;
                    }
                }
            }
            scan_offset = pos + seg_len;
            break;
        }
        pos += seg_len;
    }

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || scan_offset == 0 || num_comps == 0) {
        out.error = "Invalid or incomplete Baseline JPEG";
        return out;
    }

    // Bit reader for JPEG entropy-coded scan (handles 0xFF 0x00 byte stuffing)
    struct BitReader {
        const uint8_t* buf;
        size_t len;
        size_t pos;
        uint32_t bit_buf = 0;
        int bit_cnt = 0;

        uint8_t next_byte() {
            if (pos >= len) return 0;
            uint8_t b = buf[pos++];
            if (b == 0xFF && pos < len) {
                uint8_t next = buf[pos];
                if (next == 0x00) {
                    ++pos; // stuffed zero
                } else if (next >= 0xD0 && next <= 0xD7) {
                    ++pos; // restart marker consumed
                    return next_byte();
                } else {
                    --pos; // hit EOI or next marker
                    return 0;
                }
            }
            return b;
        }

        int read_bit() {
            if (bit_cnt == 0) {
                bit_buf = next_byte();
                bit_cnt = 8;
            }
            --bit_cnt;
            return static_cast<int>((bit_buf >> bit_cnt) & 1u);
        }

        int read_bits(int n) {
            int v = 0;
            for (int i = 0; i < n; ++i) v = (v << 1) | read_bit();
            return v;
        }

        int receive_extend(int n) {
            if (n == 0) return 0;
            int v = read_bits(n);
            int half = 1 << (n - 1);
            if (v < half) v -= (1 << n) - 1;
            return v;
        }

        int decode_huff(const HuffTable& ht) {
            uint32_t code = 0;
            size_t idx = 0;
            for (int bits = 1; bits <= 16; ++bits) {
                code = (code << 1) | static_cast<uint32_t>(read_bit());
                int cnt = ht.counts[bits - 1];
                for (int k = 0; k < cnt; ++k, ++idx) {
                    if (ht.codes[idx] == code) return ht.symbols[idx];
                }
            }
            return -1;
        }

        void align_byte() {
            bit_cnt = 0;
            bit_buf = 0;
        }
    } br{data, len, scan_offset};

    // Precompute 8x8 cosine table for separable 2D IDCT
    static float cos_tbl[8][8];
    static bool cos_init = false;
    if (!cos_init) {
        constexpr float kPi = 3.14159265358979323846f;
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) {
                float alpha = (u == 0) ? 0.35355339f : 0.5f; // 1/(2*sqrt(2)) vs 1/2
                cos_tbl[x][u] = alpha * std::cos((2.0f * x + 1.0f) * u * kPi / 16.0f);
            }
        }
        cos_init = true;
    }

    auto idct_8x8 = [&](const int coef[64], float out_px[64]) {
        float tmp[64];
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                float sum = 0.0f;
                for (int u = 0; u < 8; ++u) {
                    sum += static_cast<float>(coef[y * 8 + u]) * cos_tbl[x][u];
                }
                tmp[y * 8 + x] = sum;
            }
        }
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                float sum = 0.0f;
                for (int v = 0; v < 8; ++v) {
                    sum += tmp[v * 8 + x] * cos_tbl[y][v];
                }
                out_px[y * 8 + x] = sum + 128.0f;
            }
        }
    };

    int mcu_w = max_h * 8;
    int mcu_h = max_v * 8;
    int mcus_x = (width + mcu_w - 1) / mcu_w;
    int mcus_y = (height + mcu_h - 1) / mcu_h;

    out.width = width;
    out.height = height;
    out.argb.assign(static_cast<size_t>(width) * height, 0xFF000000u);

    // Component block storage per MCU (up to 4x4 blocks per component)
    float comp_buf[3][32 * 32];
    int mcu_count = 0;

    for (int my = 0; my < mcus_y; ++my) {
        for (int mx = 0; mx < mcus_x; ++mx) {
            if (restart_interval > 0 && mcu_count > 0 && (mcu_count % restart_interval) == 0) {
                br.align_byte();
                for (int c = 0; c < num_comps; ++c) comps[c].dc_pred = 0;
            }
            ++mcu_count;

            for (int c = 0; c < num_comps; ++c) {
                JpegComp& cp = comps[c];
                const HuffTable& dc_ht = huff_dc[cp.dc_id];
                const HuffTable& ac_ht = huff_ac[cp.ac_id];
                if (!dc_ht.valid || !ac_ht.valid || !qt_valid[cp.qt_id]) {
                    out.error = "Missing JPEG Huffman or Quantization table";
                    return out;
                }
                int cw = cp.h_samp * 8;
                for (int bv = 0; bv < cp.v_samp; ++bv) {
                    for (int bh = 0; bh < cp.h_samp; ++bh) {
                        int coef[64] = {};
                        int s = br.decode_huff(dc_ht);
                        if (s < 0) s = 0;
                        int diff = br.receive_extend(s & 0x0F);
                        cp.dc_pred += diff;
                        coef[0] = cp.dc_pred * qt[cp.qt_id][0];

                        int k = 1;
                        while (k < 64) {
                            int rs = br.decode_huff(ac_ht);
                            if (rs <= 0) break; // EOB or error
                            int run = rs >> 4;
                            int sz = rs & 0x0F;
                            if (sz == 0) {
                                if (run == 15) { k += 16; continue; }
                                break;
                            }
                            k += run;
                            if (k >= 64) break;
                            int val = br.receive_extend(sz);
                            int z = kZigZag[k];
                            coef[z] = val * qt[cp.qt_id][z];
                            ++k;
                        }

                        float block[64];
                        idct_8x8(coef, block);
                        for (int by = 0; by < 8; ++by) {
                            for (int bx = 0; bx < 8; ++bx) {
                                int py = bv * 8 + by;
                                int px = bh * 8 + bx;
                                if (py < 32 && px < 32) {
                                    comp_buf[c][py * cw + px] = block[by * 8 + bx];
                                }
                            }
                        }
                    }
                }
            }

            // Convert MCU YCbCr -> ARGB32
            for (int py = 0; py < mcu_h; ++py) {
                int gy = my * mcu_h + py;
                if (gy >= height) break;
                for (int px = 0; px < mcu_w; ++px) {
                    int gx = mx * mcu_w + px;
                    if (gx >= width) break;

                    int yx = (px * comps[0].h_samp) / max_h;
                    int yy = (py * comps[0].v_samp) / max_v;
                    float Y = comp_buf[0][yy * (comps[0].h_samp * 8) + yx];

                    uint8_t r = 0, g = 0, b = 0;
                    if (num_comps == 1) {
                        uint8_t v = static_cast<uint8_t>(std::clamp(std::round(Y), 0.0f, 255.0f));
                        r = g = b = v;
                    } else {
                        int cbx = (px * comps[1].h_samp) / max_h;
                        int cby = (py * comps[1].v_samp) / max_v;
                        float Cb = comp_buf[1][cby * (comps[1].h_samp * 8) + cbx] - 128.0f;

                        int crx = (px * comps[2].h_samp) / max_h;
                        int cry = (py * comps[2].v_samp) / max_v;
                        float Cr = comp_buf[2][cry * (comps[2].h_samp * 8) + crx] - 128.0f;

                        r = static_cast<uint8_t>(std::clamp(std::round(Y + 1.402f * Cr), 0.0f, 255.0f));
                        g = static_cast<uint8_t>(std::clamp(std::round(Y - 0.344136f * Cb - 0.714136f * Cr), 0.0f, 255.0f));
                        b = static_cast<uint8_t>(std::clamp(std::round(Y + 1.772f * Cb), 0.0f, 255.0f));
                    }
                    out.argb[static_cast<size_t>(gy * width + gx)] =
                        0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
                }
            }
        }
    }

    out.valid = true;
    return out;
}

DecodedImage ImageDecoder::decode_gif(const uint8_t* data, size_t len) {
    DecodedImage out;
    out.format = "GIF";
    if (!data || len < 14) { out.error = "Truncated GIF header"; return out; }

    int canvas_w = read_le16(data + 6);
    int canvas_h = read_le16(data + 8);
    if (canvas_w <= 0 || canvas_h <= 0 || canvas_w > 4096 || canvas_h > 4096) {
        out.error = "Invalid GIF dimensions";
        return out;
    }
    uint8_t packed = data[10];
    bool has_gct = (packed & 0x80u) != 0;
    size_t gct_entries = has_gct ? (size_t(1) << ((packed & 0x07u) + 1)) : 0;
    size_t pos = 13;
    std::vector<uint32_t> gct;
    if (has_gct) {
        if (pos + gct_entries * 3 > len) { out.error = "Truncated GIF GCT"; return out; }
        gct.resize(gct_entries);
        for (size_t i = 0; i < gct_entries; ++i) {
            gct[i] = 0xFF000000u |
                     (static_cast<uint32_t>(data[pos + i * 3]) << 16) |
                     (static_cast<uint32_t>(data[pos + i * 3 + 1]) << 8) |
                     static_cast<uint32_t>(data[pos + i * 3 + 2]);
        }
        pos += gct_entries * 3;
    }

    out.width = canvas_w;
    out.height = canvas_h;
    out.argb.assign(static_cast<size_t>(canvas_w) * static_cast<size_t>(canvas_h), 0x00000000u);

    bool has_trans = false;
    uint8_t trans_idx = 0;

    while (pos < len) {
        uint8_t intro = data[pos++];
        if (intro == 0x3Bu) break; // Trailer
        if (intro == 0x21u) { // Extension block
            if (pos >= len) break;
            uint8_t label = data[pos++];
            if (label == 0xF9u && pos + 5 <= len && data[pos] == 4) {
                uint8_t gce_flags = data[pos + 1];
                has_trans = (gce_flags & 0x01u) != 0;
                trans_idx = data[pos + 4];
            }
            // Skip sub-blocks
            while (pos < len) {
                uint8_t sz = data[pos++];
                if (sz == 0) break;
                if (pos + sz > len) { pos = len; break; }
                pos += sz;
            }
            continue;
        }
        if (intro != 0x2Cu) continue; // Wait for Image Descriptor
        if (pos + 9 > len) { out.error = "Truncated GIF image descriptor"; return out; }

        int left = read_le16(data + pos);
        int top = read_le16(data + pos + 2);
        int iw = read_le16(data + pos + 4);
        int ih = read_le16(data + pos + 6);
        uint8_t ipacked = data[pos + 8];
        pos += 9;

        bool has_lct = (ipacked & 0x80u) != 0;
        bool interlaced = (ipacked & 0x40u) != 0;
        std::vector<uint32_t> lct;
        if (has_lct) {
            size_t lct_entries = size_t(1) << ((ipacked & 0x07u) + 1);
            if (pos + lct_entries * 3 > len) { out.error = "Truncated GIF LCT"; return out; }
            lct.resize(lct_entries);
            for (size_t i = 0; i < lct_entries; ++i) {
                lct[i] = 0xFF000000u |
                         (static_cast<uint32_t>(data[pos + i * 3]) << 16) |
                         (static_cast<uint32_t>(data[pos + i * 3 + 1]) << 8) |
                         static_cast<uint32_t>(data[pos + i * 3 + 2]);
            }
            pos += lct_entries * 3;
        }
        const auto& palette = has_lct ? lct : gct;
        if (palette.empty() || pos >= len) { out.error = "Missing GIF color table"; return out; }

        uint8_t min_code_size = data[pos++];
        if (min_code_size < 2 || min_code_size > 11) { out.error = "Invalid GIF LZW code size"; return out; }

        // Gather LZW sub-blocks
        std::vector<uint8_t> lzw_bytes;
        while (pos < len) {
            uint8_t sz = data[pos++];
            if (sz == 0) break;
            if (pos + sz > len) { out.error = "Truncated GIF LZW stream"; return out; }
            lzw_bytes.insert(lzw_bytes.end(), data + pos, data + pos + sz);
            pos += sz;
        }

        // Pure-C++20 GIF LZW Decompressor (max 4096 table entries)
        const int clear_code = 1 << min_code_size;
        const int eoi_code = clear_code + 1;
        int code_size = min_code_size + 1;
        int next_code = eoi_code + 1;

        std::vector<int> prefix(4096, -1);
        std::vector<uint8_t> suffix(4096, 0);
        for (int i = 0; i < clear_code; ++i) suffix[i] = static_cast<uint8_t>(i);

        size_t bit_pos = 0;
        const size_t total_bits = lzw_bytes.size() * 8;
        auto read_code = [&](int width_bits) -> int {
            if (bit_pos + static_cast<size_t>(width_bits) > total_bits) return eoi_code;
            int val = 0;
            for (int b = 0; b < width_bits; ++b) {
                size_t bp = bit_pos + static_cast<size_t>(b);
                if ((lzw_bytes[bp >> 3] >> (bp & 7)) & 1u) val |= (1 << b);
            }
            bit_pos += static_cast<size_t>(width_bits);
            return val;
        };

        std::vector<uint8_t> indices;
        const size_t max_pixels = static_cast<size_t>(iw) * static_cast<size_t>(ih);
        indices.reserve(std::min<size_t>(max_pixels, 1024 * 1024));
        std::vector<uint8_t> stack;
        stack.reserve(4096);

        int prev_code = -1;
        uint8_t first_char = 0;
        while (indices.size() < max_pixels) {
            int code = read_code(code_size);
            if (code == eoi_code) break;
            if (code == clear_code) {
                code_size = min_code_size + 1;
                next_code = eoi_code + 1;
                prev_code = -1;
                continue;
            }
            int in_code = code;
            stack.clear();
            if (code >= next_code) {
                if (prev_code < 0 || code > next_code) break; // Corrupt stream guard
                stack.push_back(first_char);
                code = prev_code;
            }
            int guard = 0;
            while (code >= clear_code && guard < 4096) {
                stack.push_back(suffix[code]);
                code = prefix[code];
                if (code < 0 || code >= 4096) break;
                ++guard;
            }
            if (code < 0 || code >= clear_code) break;
            first_char = static_cast<uint8_t>(code);
            stack.push_back(first_char);
            for (auto it = stack.rbegin(); it != stack.rend() && indices.size() < max_pixels; ++it) {
                indices.push_back(*it);
            }
            if (prev_code >= 0 && next_code < 4096) {
                prefix[next_code] = prev_code;
                suffix[next_code] = first_char;
                ++next_code;
                if (next_code == (1 << code_size) && code_size < 12) ++code_size;
            }
            prev_code = in_code;
        }

        // Map decoded indices onto canvas (handling optional interlacing & transparency)
        static const int kPassStart[4] = {0, 4, 2, 1};
        static const int kPassStep[4] = {8, 8, 4, 2};
        size_t src_i = 0;
        auto write_row = [&](int ry) {
            int gy = top + ry;
            for (int rx = 0; rx < iw && src_i < indices.size(); ++rx, ++src_i) {
                int gx = left + rx;
                if (gy < 0 || gy >= canvas_h || gx < 0 || gx >= canvas_w) continue;
                uint8_t idx = indices[src_i];
                if (has_trans && idx == trans_idx) continue;
                if (idx < palette.size()) {
                    out.argb[static_cast<size_t>(gy * canvas_w + gx)] = palette[idx];
                }
            }
        };
        if (interlaced) {
            for (int p = 0; p < 4; ++p) {
                for (int ry = kPassStart[p]; ry < ih; ry += kPassStep[p]) write_row(ry);
            }
        } else {
            for (int ry = 0; ry < ih; ++ry) write_row(ry);
        }

        out.valid = !indices.empty();
        break; // First frame decoded
    }
    return out;
}

DecodedImage ImageDecoder::decode_webp(const uint8_t* data, size_t len) {
    DecodedImage out;
    out.format = "WEBP";
    if (!data || len < 20) { out.error = "Truncated WebP buffer"; return out; }

    // Runtime dlopen("libwebp.so.7" / "libwebp.so.6") for full VP8/VP8L/VP8X decoding with zero compile deps
    using DecodeRGBAFn = uint8_t* (*)(const uint8_t*, size_t, int*, int*);
    using WebPFreeFn = void (*)(void*);
    static void* webp_lib = nullptr;
    static DecodeRGBAFn fn_decode = nullptr;
    static WebPFreeFn fn_free = nullptr;
    static bool tried_dl = false;
    if (!tried_dl) {
        tried_dl = true;
        for (const char* name : platform::libwebp_names()) {
            webp_lib = dlopen(name, RTLD_LAZY);
            if (webp_lib) break;
        }
        if (webp_lib) {
            fn_decode = reinterpret_cast<DecodeRGBAFn>(dlsym(webp_lib, "WebPDecodeRGBA"));
            fn_free = reinterpret_cast<WebPFreeFn>(dlsym(webp_lib, "WebPFree"));
        }
    }
    if (fn_decode) {
        int w = 0, h = 0;
        uint8_t* rgba = fn_decode(data, len, &w, &h);
        if (rgba && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            out.width = w;
            out.height = h;
            out.argb.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
            for (size_t i = 0; i < out.argb.size(); ++i) {
                uint8_t r = rgba[i * 4 + 0];
                uint8_t g = rgba[i * 4 + 1];
                uint8_t b = rgba[i * 4 + 2];
                uint8_t a = rgba[i * 4 + 3];
                out.argb[i] = (static_cast<uint32_t>(a) << 24) |
                              (static_cast<uint32_t>(r) << 16) |
                              (static_cast<uint32_t>(g) << 8) | b;
            }
            if (fn_free) fn_free(rgba);
            else std::free(rgba);
            out.valid = true;
            return out;
        }
        if (rgba) {
            if (fn_free) fn_free(rgba);
            else std::free(rgba);
        }
    }

    // Pure-C++20 fallback: probe VP8X / VP8L / VP8 chunk dimensions cleanly
    int w = 0, h = 0;
    if (len >= 30 && std::memcmp(data + 12, "VP8X", 4) == 0) {
        w = 1 + (data[24] | (data[25] << 8) | (data[26] << 16));
        h = 1 + (data[27] | (data[28] << 8) | (data[29] << 16));
    } else if (len >= 25 && std::memcmp(data + 12, "VP8L", 4) == 0 && data[20] == 0x2Fu) {
        uint32_t bits = read_le32(data + 21);
        w = static_cast<int>((bits & 0x3FFFu) + 1);
        h = static_cast<int>(((bits >> 14) & 0x3FFFu) + 1);
    } else if (len >= 30 && std::memcmp(data + 12, "VP8 ", 4) == 0 &&
               data[23] == 0x9Du && data[24] == 0x01u && data[25] == 0x2Au) {
        w = read_le16(data + 26) & 0x3FFFu;
        h = read_le16(data + 28) & 0x3FFFu;
    }
    if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        out.width = w;
        out.height = h;
        out.error = "libwebp not installed on host (container header parsed: " + std::to_string(w) + "x" + std::to_string(h) + ")";
    } else {
        out.error = "Invalid WebP bitstream";
    }
    return out;
}

} // namespace astra
