#pragma once

#include "dom/html_parser.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    uint32_t to_argb() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    }

    static Color from_argb(uint32_t v) {
        return Color{
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>(v & 0xFF),
            static_cast<uint8_t>((v >> 24) & 0xFF)
        };
    }
};

enum class DisplayType : uint8_t {
    Block,
    Inline,
    Flex,
    Grid,
    None
};

enum class FlexDirection : uint8_t {
    Row,
    Column
};

struct GridTrack {
    float value = 1.0f;
    bool is_fr = true;
};

struct EdgeSizes {
    float top = 0;
    float right = 0;
    float bottom = 0;
    float left = 0;
};

struct ComputedStyle {
    DisplayType display = DisplayType::Block;
    FlexDirection flex_direction = FlexDirection::Row;
    std::vector<GridTrack> grid_columns;
    float gap = 0.0f;
    float width = -1.0f;   // < 0 means auto
    float height = -1.0f;  // < 0 means auto
    EdgeSizes margin;
    EdgeSizes padding;
    float border_width = 0.0f;
    float border_radius = 0.0f;
    Color border_color{0, 0, 0, 0};
    Color background{0, 0, 0, 0};
    Color color{230, 234, 240, 255};
    float font_size = 14.0f;
    bool bold = false;
    bool monospace = false;
    float line_height = 1.48f;
};

struct CssDeclaration {
    std::string property;
    std::string value;
};

struct CssRule {
    std::vector<std::string> selectors;
    std::vector<CssDeclaration> declarations;
    size_t source_order = 0;
};

class CssStylesheet {
public:
    void parse(std::string_view css);
    const std::vector<CssRule>& rules() const { return rules_; }

private:
    std::vector<CssRule> rules_;
};

class StyleEngine {
public:
    static Color parse_color(std::string_view val, Color fallback);
    static float parse_px(std::string_view val, float fallback = 0.0f);

    static ComputedStyle compute_style(const DomNode* node,
                                       const CssStylesheet& sheet,
                                       const ComputedStyle* parent_style);
};

} // namespace astra
