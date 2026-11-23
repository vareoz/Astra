#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace astra {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::string format; // "PNG", "BMP", "PPM"
    std::vector<uint32_t> argb;
    bool valid = false;
    std::string error;
};

// Zero-build-dependency native image decoder (inspired by Firefox image/decoders/).
// Supports:
//   - PNG (8-bit RGB, RGBA, Grayscale, Grayscale+Alpha, Indexed PLTE; all 5 PNG filters)
//   - BMP (24-bit & 32-bit uncompressed DIB)
//   - PPM (P6 binary RGB)
class ImageDecoder {
public:
    static DecodedImage decode_bytes(const uint8_t* data, size_t len);
    static DecodedImage decode_file(const std::string& path);

private:
    static DecodedImage decode_png(const uint8_t* data, size_t len);
    static DecodedImage decode_bmp(const uint8_t* data, size_t len);
    static DecodedImage decode_ppm(const uint8_t* data, size_t len);
};

} // namespace astra
