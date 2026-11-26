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
    InlineBlock,
    Flex,
    Grid,
    Table,
    TableRow,
    TableCell,
    None
};

enum class FloatMode : uint8_t {
    None,
    Left,
    Right
};

enum class ClearMode : uint8_t {
    None,
    Left,
    Right,
    Both
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
    // `minmax(min, max)` con un max en longitud: la pista NO es rígida, puede
    // encogerse si el contenedor no da para tanto.
    float max_px = -1.0f;
    // `min-content` / `max-content` / `fit-content` / `auto`: el tamaño sale
    // del contenido, no del reparto del espacio libre.
    bool content_sized = false;
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
    // CSS Flexbox Level 1 §7: factor de crecimiento del ítem y tamaño base.
    // `flex-grow` reparte el espacio libre del contenedor; sin él, un
    // `flex: 1` o `flex-grow: 1` no empujaba nada y los bloques quedaban
    // amontonados a la izquierda.
    float flex_grow = 0.0f;
    float flex_basis = -1.0f; // < 0 = auto
    bool flex_wrap = false;
    JustifyContent justify_content = JustifyContent::FlexStart;
    AlignItems align_items = AlignItems::Stretch;
    TextAlign text_align = TextAlign::Left;
    PositionType position = PositionType::Static;
    FloatMode float_mode = FloatMode::None;
    ClearMode clear_mode = ClearMode::None;
    OverflowMode overflow = OverflowMode::Visible;
    BoxSizing box_sizing = BoxSizing::ContentBox;
    std::vector<GridTrack> grid_columns;
    // Session 11 — CSS Grid Level 1 §7.3: plantilla de áreas con nombre del
    // contenedor (`grid-template-areas`), una fila por cada cadena, y nombre
    // del área que ocupa este elemento (`grid-area`). Es el mecanismo con el
    // que las skins modernas colocan barra lateral y contenido en columnas.
    std::vector<std::vector<std::string>> grid_areas;
    std::string grid_area;
    float gap = 0.0f;
    // `column-gap` / `row-gap` sobreescriben a `gap` en su eje cuando se
    // declaran por separado; -1 significa "usa el valor de `gap`".
    float column_gap = -1.0f;
    float row_gap = -1.0f;
    float width = -1.0f;      // < 0 means auto
    float width_pct = -1.0f;  // >= 0 means percentage of containing width
    std::string width_calc;   // non-empty means calc(...) expression resolved against containing width
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
    float translate_x = 0.0f;
    float translate_y = 0.0f;
    int z_index = 0;
    float opacity = 1.0f;
    EdgeSizes margin;
    EdgeSizes padding;
    float border_width = 0.0f;
    float border_radius = 0.0f;
    Color border_color{0, 0, 0, 0};
    float border_left_width = 0.0f;
    Color border_left_color{0, 0, 0, 0};
    // `mask-image` recorta el fondo con la forma de una imagen. El motor aún
    // no la aplica, pero sí registra su presencia: pintar el fondo entero de
    // un elemento enmascarado produce un bloque de color macizo donde debería
    // verse un icono, así que el compositor lo omite (ver has_unapplied_mask).
    std::string mask_image;
    // URL de `background-image: url(...)`. Se pinta como imagen sobre la caja;
    // sin ella, un icono declarado así quedaba como un bloque de color.
    std::string background_image;
    bool has_unapplied_mask() const { return !mask_image.empty(); }

    Color background{0, 0, 0, 0};
    LinearGradient background_gradient;
    Color color{230, 234, 240, 255};
    BoxShadow box_shadow;
    float font_size = 14.0f;
    // Tamaño de fuente HEREDADO del padre. CSS Fonts §3.1: un `font-size` en
    // porcentaje o en `em` se resuelve contra este valor, no contra el propio
    // ni contra el del documento raíz.
    float parent_font_size = 16.0f;
    bool bold = false;
    bool monospace = false;
    float line_height = 1.48f;
    bool has_content = false;
    std::string content_text;
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
    static float eval_calc(std::string_view val, float ref_px, float viewport_w = 1280.0f, float fallback = 0.0f);
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
                                       bool light_web_default = false,
                                       const StyleAdjuster* adjuster = nullptr);

    static bool compute_pseudo_style(const DomNode* node,
                                     const CssStylesheet& sheet,
                                     std::string_view pseudo_name, // "::before" or "::after"
                                     const ComputedStyle& element_style,
                                     ComputedStyle& out_style,
                                     float viewport_w = 1280.0f);
};

} // namespace astra
