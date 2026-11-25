#pragma once

#include "dom/html_parser.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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

enum class JustifyContent : uint8_t {
    FlexStart,
    Center,
    FlexEnd,
    SpaceBetween
};

enum class AlignItems : uint8_t {
    Stretch,
    FlexStart,
    Center,
    FlexEnd
};

enum class TextAlign : uint8_t {
    Left,
    Center,
    Right
};

struct GridTrack {
    float value = 1.0f;
    bool is_fr = true;
};

enum class PositionType : uint8_t {
    Static,
    Relative,
    Absolute,
    Fixed
};

enum class OverflowMode : uint8_t {
    Visible,
    Hidden
};

enum class BoxSizing : uint8_t {
    ContentBox,
    BorderBox
};

struct LinearGradient {
    bool enabled = false;
    float angle_deg = 180.0f; // CSS default: top-to-bottom (180deg)
    Color start{15, 23, 42, 255};
    Color end{30, 41, 59, 255};
};

struct BoxShadow {
    bool enabled = false;
    float offset_x = 0.0f;
    float offset_y = 6.0f;
    float blur = 12.0f;
    Color color{0, 0, 0, 110};
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
    JustifyContent justify_content = JustifyContent::FlexStart;
    AlignItems align_items = AlignItems::Stretch;
    TextAlign text_align = TextAlign::Left;
    PositionType position = PositionType::Static;
    OverflowMode overflow = OverflowMode::Visible;
    BoxSizing box_sizing = BoxSizing::ContentBox;
    std::vector<GridTrack> grid_columns;
    float gap = 0.0f;
    float width = -1.0f;      // < 0 means auto
    float height = -1.0f;     // < 0 means auto
    float min_width = -1.0f;
    float max_width = -1.0f;
    bool has_top = false;
    bool has_left = false;
    bool has_right = false;
    bool has_bottom = false;
    float top = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    int z_index = 0;
    float opacity = 1.0f;
    EdgeSizes margin;
    EdgeSizes padding;
    float border_width = 0.0f;
    float border_radius = 0.0f;
    Color border_color{0, 0, 0, 0};
    Color background{0, 0, 0, 0};
    LinearGradient background_gradient;
    Color color{230, 234, 240, 255};
    BoxShadow box_shadow;
    float font_size = 14.0f;
    bool bold = false;
    bool monospace = false;
    float line_height = 1.48f;
    std::unordered_map<std::string, std::string> css_vars;
};

struct CssDeclaration {
    std::string property;
    std::string value;
};

struct CssRule {
    std::vector<std::string> selectors;
    std::vector<CssDeclaration> declarations;
    size_t source_order = 0;
    float media_min_width = -1.0f;
    float media_max_width = -1.0f;
};

class CssStylesheet {
public:
    void parse(std::string_view css);
    const std::vector<CssRule>& rules() const { return rules_; }

private:
    void parse_block(std::string_view css, float min_w, float max_w);
    std::vector<CssRule> rules_;
};

class StyleEngine {
public:
    static Color parse_color(std::string_view val, Color fallback);
    static float parse_px(std::string_view val, float fallback = 0.0f);
    static std::string resolve_var_expr(std::string_view val,
                                        const std::unordered_map<std::string, std::string>& vars,
                                        int depth = 0);
    static std::unordered_map<std::string, std::string> collect_root_vars(const CssStylesheet& sheet,
                                                                          float viewport_w = 1280.0f);

    static ComputedStyle compute_style(const DomNode* node,
                                       const CssStylesheet& sheet,
                                       const ComputedStyle* parent_style,
                                       float viewport_w = 1280.0f);
};

} // namespace astra
