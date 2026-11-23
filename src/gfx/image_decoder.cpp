#include "gfx/image_decoder.hpp"
#include "net/http_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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
    if (data[0] == 'B' && data[1] == 'M' && len >= 54) {
        return decode_bmp(data, len);
    }
    if (data[0] == 'P' && data[1] == '6') {
        return decode_ppm(data, len);
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

} // namespace astra
