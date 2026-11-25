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

struct CssDeclaration {
    std::string property;
    std::string value;

    bool operator==(const CssDeclaration& o) const {
        return property == o.property && value == o.value;
    }
    bool operator!=(const CssDeclaration& o) const { return !(*this == o); }
};

// =============================================================================
// Session 8 — AstraStyle v8: transformaciones 2D y animaciones web
// =============================================================================

// Transformación afín 2D guardada en forma descompuesta (traslación, rotación,
// escala) en lugar de una matriz cerrada: así el motor de animaciones puede
// interpolar cada componente por separado, que es exactamente lo que exige CSS
// cuando las listas de funciones de origen y destino coinciden.
struct Transform2D {
    bool enabled = false;
    float translate_x = 0.0f;
    float translate_y = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float rotate_deg = 0.0f;

    bool is_identity() const {
        return translate_x == 0.0f && translate_y == 0.0f &&
               scale_x == 1.0f && scale_y == 1.0f && rotate_deg == 0.0f;
    }

    // Construye la matriz afín [a c e; b d f] alrededor de (ox, oy), que es el
    // centro de la caja por defecto (`transform-origin: 50% 50%`).
    // Orden de composición: Traslación * Rotación * Escala.
    void to_matrix(float ox, float oy, float out[6]) const;
};

enum class EasingKind : uint8_t {
    Linear,
    CubicBezier,   // cubre ease / ease-in / ease-out / ease-in-out / cubic-bezier()
    StepStart,
    StepEnd
};

// Función de temporización CSS. Las palabras clave se traducen a sus puntos de
// control oficiales (CSS Easing Functions Level 1).
struct Easing {
    EasingKind kind = EasingKind::CubicBezier;
    float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.0f; // ease
    int steps = 1;

    // Evalúa la curva en el progreso lineal t (0..1). Definida en
    // style/animation_engine.cpp (resolución Newton-Raphson + bisección).
    float eval(float t) const;

    static Easing linear() { return Easing{EasingKind::Linear, 0, 0, 1, 1, 1}; }
    static Easing ease() { return Easing{EasingKind::CubicBezier, 0.25f, 0.1f, 0.25f, 1.0f, 1}; }
    static Easing ease_in() { return Easing{EasingKind::CubicBezier, 0.42f, 0.0f, 1.0f, 1.0f, 1}; }
    static Easing ease_out() { return Easing{EasingKind::CubicBezier, 0.0f, 0.0f, 0.58f, 1.0f, 1}; }
    static Easing ease_in_out() { return Easing{EasingKind::CubicBezier, 0.42f, 0.0f, 0.58f, 1.0f, 1}; }
    static Easing parse(std::string_view value, Easing fallback = Easing::ease());
};

enum class AnimationDirection : uint8_t {
    Normal,
    Reverse,
    Alternate,
    AlternateReverse
};

enum class AnimationFillMode : uint8_t {
    None,
    Forwards,
    Backwards,
    Both
};

// Atajo `animation: <name> <duration> <easing> <delay> <count> <direction> <fill>`
// con orden libre, más todas sus propiedades longhand.
struct AnimationSpec {
    std::string name;
    float duration_ms = 0.0f;
    float delay_ms = 0.0f;
    float iteration_count = 1.0f; // < 0 => infinite
    AnimationDirection direction = AnimationDirection::Normal;
    AnimationFillMode fill_mode = AnimationFillMode::None;
    Easing easing = Easing::ease();

    bool valid() const { return !name.empty() && duration_ms > 0.0f; }
    bool infinite() const { return iteration_count < 0.0f; }
};

// Una entrada de la lista `transition` (separada por comas).
struct TransitionSpec {
    std::string property = "all"; // "all" cubre todas las propiedades animables
    float duration_ms = 0.0f;
    float delay_ms = 0.0f;
    Easing easing = Easing::ease();

    bool matches(std::string_view prop) const {
        return property == "all" || property == prop;
    }
};

// Un paso de @keyframes ya normalizado a offset 0..1 ("from" => 0, "to" => 1).
struct CssKeyframe {
    float offset = 0.0f;
    std::vector<CssDeclaration> declarations;
};

struct CssKeyframesRule {
    std::string name;
    std::vector<CssKeyframe> keyframes; // ordenados por offset ascendente
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
    Transform2D transform;
    AnimationSpec animation;
    std::vector<TransitionSpec> transitions;
    std::unordered_map<std::string, std::string> css_vars;
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

    // Session 8: reglas @keyframes recogidas durante el parseo.
    const std::vector<CssKeyframesRule>& keyframes() const { return keyframes_; }
    const CssKeyframesRule* find_keyframes(std::string_view name) const;

private:
    void parse_block(std::string_view css, float min_w, float max_w);
    void parse_keyframes_block(std::string_view header, std::string_view body);
    std::vector<CssRule> rules_;
    std::vector<CssKeyframesRule> keyframes_;
};

// Session 8: gancho que permite al motor de animaciones inyectar en la cascada
// las declaraciones que produce el efecto animado, con la prioridad que exige
// CSS (por encima de las reglas del autor y del atributo `style`), sin que
// style/css_engine dependa de style/animation_engine.
class StyleAdjuster {
public:
    virtual ~StyleAdjuster() = default;
    virtual void collect(const DomNode* node, std::vector<CssDeclaration>& out) const = 0;
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

    static Transform2D parse_transform(std::string_view val);
    static float parse_time_ms(std::string_view val, float fallback = 0.0f);

    static ComputedStyle compute_style(const DomNode* node,
                                       const CssStylesheet& sheet,
                                       const ComputedStyle* parent_style,
                                       float viewport_w = 1280.0f,
                                       const StyleAdjuster* adjuster = nullptr);
};

} // namespace astra
