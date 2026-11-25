#pragma once

// =============================================================================
// AstraAnim — Motor de animaciones web (Session 8)
//
// Implementa en C++20 puro las dos formas de animación declarativa de CSS:
//
//   1. CSS Animations Level 1 — reglas `@keyframes` activadas por la propiedad
//      `animation`, con duración, retardo, número de iteraciones (incluido
//      `infinite`), dirección (`normal` / `reverse` / `alternate` /
//      `alternate-reverse`), modo de relleno (`forwards` / `backwards` /
//      `both`) y función de temporización.
//
//   2. CSS Transitions Level 1 — la propiedad `transition`, que interpola el
//      salto entre el valor antiguo y el nuevo de una propiedad cuando el
//      estilo calculado cambia (por ejemplo desde `element.style.*` en
//      AstraJS, o al añadir una clase).
//
// El reloj es el mismo `requestAnimationFrame` que ya conduce el compositor:
// `AnimationTimeline::advance(now_ms)` muestrea todas las animaciones activas
// y produce declaraciones CSS interpoladas que `StyleEngine::compute_style`
// inyecta al final de la cascada a través de `StyleAdjuster`.
//
// La interpolación es consciente del tipo de valor: longitudes en px, números
// sin unidad, colores (#rgb / #rrggbb / rgb() / rgba() / nombres) y listas de
// funciones `transform` componente a componente.
// =============================================================================

#include "dom/html_parser.hpp"
#include "style/css_engine.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace astra {

// Resultado de muestrear una animación en un instante concreto.
struct AnimationSample {
    bool active = false;          // la animación está en su fase de reproducción
    bool finished = false;        // ya consumió todas sus iteraciones
    float progress = 0.0f;        // progreso de la iteración actual tras easing (0..1)
    int64_t iteration = 0;        // índice de la iteración en curso
    std::vector<CssDeclaration> declarations;
};

// Interpolación de valores CSS por tipo.
class ValueInterpolator {
public:
    // Devuelve el valor interpolado entre `from` y `to` para `property` con
    // t en 0..1. Si los valores no son interpolables devuelve `to` cuando
    // t >= 0.5 y `from` en caso contrario (comportamiento discreto de CSS).
    static std::string interpolate(std::string_view property,
                                   std::string_view from,
                                   std::string_view to,
                                   float t);

    // true si la propiedad admite interpolación continua en este motor.
    static bool is_animatable(std::string_view property);

private:
    static bool looks_like_color(std::string_view v);
    static bool looks_numeric(std::string_view v);
};

// Línea de tiempo global de un documento: mantiene el estado de las
// animaciones @keyframes y de las transiciones en curso.
class AnimationTimeline : public StyleAdjuster {
public:
    // Vincula la hoja de estilo que contiene las reglas @keyframes. No toma
    // posesión: la hoja debe sobrevivir a la línea de tiempo.
    void attach(const CssStylesheet* sheet) { sheet_ = sheet; }
    const CssStylesheet* sheet() const { return sheet_; }

    // Registra la animación declarada por un elemento. Idempotente: si el
    // nombre y la duración no cambian, conserva el instante de inicio para que
    // la animación no se reinicie en cada relayout.
    void register_animation(const DomNode* node, const AnimationSpec& spec, double now_ms);

    // Declara las transiciones de un elemento y detecta los cambios de valor
    // que deben animarse. `resolved` son los valores base (sin animación) de
    // las propiedades que el elemento declara como transicionables.
    void register_transitions(const DomNode* node,
                              const std::vector<TransitionSpec>& specs,
                              const std::map<std::string, std::string>& resolved,
                              double now_ms);

    // Avanza el reloj. Devuelve true si algún valor animado cambió, es decir,
    // si hace falta recalcular estilo y layout en este fotograma.
    bool advance(double now_ms);

    // StyleAdjuster: aporta las declaraciones animadas del elemento.
    void collect(const DomNode* node, std::vector<CssDeclaration>& out) const override;

    // Serializa a texto CSS las propiedades animables de un estilo calculado.
    // Las transiciones de CSS operan sobre valores calculados, así que este es
    // el "antes/después" que detecta los cambios que deben interpolarse.
    static std::map<std::string, std::string> snapshot(const ComputedStyle& st);

    // Muestreo puntual de una animación @keyframes (sin estado): útil para
    // pruebas y para el inspector.
    AnimationSample sample_keyframes(const AnimationSpec& spec,
                                     double elapsed_ms) const;

    // Elimina el estado de nodos que ya no están en el árbol.
    void retain_only(const HtmlDocument& doc);
    void clear();

    size_t active_animation_count() const;
    size_t active_transition_count() const;
    double clock_ms() const { return clock_ms_; }

private:
    struct AnimationState {
        AnimationSpec spec;
        double start_ms = 0.0;
        bool finished = false;
        std::vector<CssDeclaration> current;
    };

    struct TransitionState {
        std::string property;
        std::string from;
        std::string to;
        double start_ms = 0.0;
        float duration_ms = 0.0f;
        float delay_ms = 0.0f;
        Easing easing;
        bool finished = false;
        std::string current;
    };

    struct NodeState {
        AnimationState animation;
        bool has_animation = false;
        std::vector<TransitionState> transitions;
        std::map<std::string, std::string> last_base_values;
    };

    const CssStylesheet* sheet_ = nullptr;
    double clock_ms_ = 0.0;
    std::unordered_map<const DomNode*, NodeState> states_;
};

} // namespace astra
