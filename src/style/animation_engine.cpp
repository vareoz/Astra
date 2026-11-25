#include "style/animation_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace astra {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// Formatea sin ceros finales para que los valores interpolados vuelvan a ser
// cadenas CSS limpias ("12.5px", "0.37", "rotate(18deg)").
std::string fmt_num(float v) {
    if (std::fabs(v - std::round(v)) < 1e-4f) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Extrae el número inicial y la unidad de una longitud CSS ("12.5px" -> 12.5, "px").
bool split_number_unit(std::string_view v, float& num, std::string& unit) {
    std::string s = trim(v);
    if (s.empty()) return false;
    char* end = nullptr;
    double parsed = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    num = static_cast<float>(parsed);
    unit = trim(std::string(end));
    return true;
}

struct Rgba {
    float r = 0, g = 0, b = 0, a = 255;
};

bool parse_rgba(std::string_view value, Rgba& out) {
    std::string v = to_lower(trim(value));
    if (v.empty()) return false;

    if (v[0] == '#') {
        std::string hex = v.substr(1);
        auto hv = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        for (char c : hex) {
            if (hv(c) < 0) return false;
        }
        if (hex.size() == 3 || hex.size() == 4) {
            out.r = static_cast<float>(hv(hex[0]) * 17);
            out.g = static_cast<float>(hv(hex[1]) * 17);
            out.b = static_cast<float>(hv(hex[2]) * 17);
            out.a = (hex.size() == 4) ? static_cast<float>(hv(hex[3]) * 17) : 255.0f;
            return true;
        }
        if (hex.size() == 6 || hex.size() == 8) {
            out.r = static_cast<float>(hv(hex[0]) * 16 + hv(hex[1]));
            out.g = static_cast<float>(hv(hex[2]) * 16 + hv(hex[3]));
            out.b = static_cast<float>(hv(hex[4]) * 16 + hv(hex[5]));
            out.a = (hex.size() == 8) ? static_cast<float>(hv(hex[6]) * 16 + hv(hex[7])) : 255.0f;
            return true;
        }
        return false;
    }

    if (v.rfind("rgb", 0) == 0) {
        size_t lp = v.find('(');
        size_t rp = v.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp <= lp) return false;
        std::string inner = v.substr(lp + 1, rp - lp - 1);
        float comps[4] = {0, 0, 0, 1.0f};
        int n = 0;
        size_t start = 0;
        while (n < 4 && start <= inner.size()) {
            size_t comma = inner.find_first_of(",/", start);
            std::string piece = trim(comma == std::string::npos ? inner.substr(start)
                                                                : inner.substr(start, comma - start));
            if (!piece.empty()) comps[n++] = static_cast<float>(std::atof(piece.c_str()));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (n < 3) return false;
        out.r = comps[0];
        out.g = comps[1];
        out.b = comps[2];
        out.a = (n >= 4) ? comps[3] * 255.0f : 255.0f;
        return true;
    }

    // Colores nombrados que el motor ya reconoce en la cascada.
    struct Named { const char* name; uint8_t r, g, b; };
    static const Named kNamed[] = {
        {"black", 0, 0, 0},        {"white", 255, 255, 255},
        {"red", 255, 0, 0},        {"lime", 0, 255, 0},
        {"green", 0, 128, 0},      {"blue", 0, 0, 255},
        {"yellow", 255, 255, 0},   {"cyan", 0, 255, 255},
        {"magenta", 255, 0, 255},  {"gray", 128, 128, 128},
        {"grey", 128, 128, 128},   {"orange", 255, 165, 0},
        {"navy", 0, 0, 128},       {"teal", 0, 128, 128},
        {"silver", 192, 192, 192}, {"transparent", 0, 0, 0}
    };
    for (const auto& nm : kNamed) {
        if (v == nm.name) {
            out.r = nm.r;
            out.g = nm.g;
            out.b = nm.b;
            out.a = (v == "transparent") ? 0.0f : 255.0f;
            return true;
        }
    }
    return false;
}

std::string format_rgba(const Rgba& c) {
    auto clamp255 = [](float v) { return std::clamp(v, 0.0f, 255.0f); };
    if (c.a >= 254.5f) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                      static_cast<int>(std::lround(clamp255(c.r))),
                      static_cast<int>(std::lround(clamp255(c.g))),
                      static_cast<int>(std::lround(clamp255(c.b))));
        return buf;
    }
    return "rgba(" + fmt_num(std::round(clamp255(c.r))) + ", " +
           fmt_num(std::round(clamp255(c.g))) + ", " +
           fmt_num(std::round(clamp255(c.b))) + ", " +
           fmt_num(std::clamp(c.a / 255.0f, 0.0f, 1.0f)) + ")";
}

std::string format_transform(const Transform2D& t) {
    std::string out;
    if (t.translate_x != 0.0f || t.translate_y != 0.0f) {
        out += "translate(" + fmt_num(t.translate_x) + "px, " + fmt_num(t.translate_y) + "px)";
    }
    if (t.rotate_deg != 0.0f) {
        if (!out.empty()) out += " ";
        out += "rotate(" + fmt_num(t.rotate_deg) + "deg)";
    }
    if (t.scale_x != 1.0f || t.scale_y != 1.0f) {
        if (!out.empty()) out += " ";
        out += "scale(" + fmt_num(t.scale_x) + ", " + fmt_num(t.scale_y) + ")";
    }
    return out.empty() ? "none" : out;
}

} // namespace

// =============================================================================
// Easing::eval — resolución de la curva cúbica de Bézier de CSS
// =============================================================================
// CSS define la temporización como una Bézier cúbica con P0=(0,0) y P3=(1,1):
// hay que invertir x(t)=progreso para obtener el parámetro y evaluar y(t).
// Se usa Newton-Raphson (rápido) con respaldo de bisección cuando la derivada
// es casi nula, que es el método de los motores de producción.
float Easing::eval(float t) const {
    t = std::clamp(t, 0.0f, 1.0f);

    switch (kind) {
        case EasingKind::Linear:
            return t;
        case EasingKind::StepStart: {
            int n = std::max(1, steps);
            float v = std::ceil(t * static_cast<float>(n)) / static_cast<float>(n);
            return std::clamp(v, 0.0f, 1.0f);
        }
        case EasingKind::StepEnd: {
            int n = std::max(1, steps);
            float v = std::floor(t * static_cast<float>(n)) / static_cast<float>(n);
            return std::clamp(v, 0.0f, 1.0f);
        }
        case EasingKind::CubicBezier:
            break;
    }

    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;

    const float cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    auto bez = [](float a1, float a2, float u) {
        const float inv = 1.0f - u;
        return 3.0f * inv * inv * u * a1 + 3.0f * inv * u * u * a2 + u * u * u;
    };
    auto bez_dx = [&](float u) {
        const float inv = 1.0f - u;
        return 3.0f * inv * inv * cx1 + 6.0f * inv * u * (cx2 - cx1) + 3.0f * u * u * (1.0f - cx2);
    };

    float u = t;
    for (int i = 0; i < 8; ++i) {
        float x = bez(cx1, cx2, u) - t;
        if (std::fabs(x) < 1e-6f) return bez(cy1, cy2, u);
        float dx = bez_dx(u);
        if (std::fabs(dx) < 1e-6f) break;
        u -= x / dx;
        u = std::clamp(u, 0.0f, 1.0f);
    }

    float lo = 0.0f, hi = 1.0f;
    u = t;
    for (int i = 0; i < 32; ++i) {
        float x = bez(cx1, cx2, u);
        if (std::fabs(x - t) < 1e-6f) break;
        if (x < t) lo = u;
        else hi = u;
        u = 0.5f * (lo + hi);
    }
    return bez(cy1, cy2, u);
}

// =============================================================================
// ValueInterpolator
// =============================================================================

bool ValueInterpolator::looks_like_color(std::string_view v) {
    Rgba dummy;
    return parse_rgba(v, dummy);
}

bool ValueInterpolator::looks_numeric(std::string_view v) {
    float n = 0.0f;
    std::string unit;
    if (!split_number_unit(v, n, unit)) return false;
    return unit.empty() || unit == "px" || unit == "%" || unit == "em" ||
           unit == "rem" || unit == "vw" || unit == "vh" || unit == "deg";
}

bool ValueInterpolator::is_animatable(std::string_view property) {
    static const char* const kAnimatable[] = {
        "opacity", "width", "height", "min-width", "max-width",
        "top", "left", "right", "bottom",
        "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
        "background", "background-color", "color", "border-color",
        "border-width", "border-radius", "font-size", "line-height",
        "gap", "transform", "z-index"
    };
    std::string p = to_lower(property);
    for (const char* a : kAnimatable) {
        if (p == a) return true;
    }
    return false;
}

std::string ValueInterpolator::interpolate(std::string_view property,
                                           std::string_view from,
                                           std::string_view to,
                                           float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    std::string prop = to_lower(property);
    std::string f = trim(from);
    std::string g = trim(to);

    if (f == g) return g;
    if (t <= 0.0f) return f;
    if (t >= 1.0f) return g;

    // transform: se interpola la descomposición (traslación, rotación, escala),
    // que es el camino rápido de CSS cuando ambas listas son compatibles.
    if (prop == "transform") {
        Transform2D a = StyleEngine::parse_transform(f);
        Transform2D b = StyleEngine::parse_transform(g);
        Transform2D r;
        r.translate_x = lerp(a.translate_x, b.translate_x, t);
        r.translate_y = lerp(a.translate_y, b.translate_y, t);
        r.scale_x = lerp(a.scale_x, b.scale_x, t);
        r.scale_y = lerp(a.scale_y, b.scale_y, t);
        r.rotate_deg = lerp(a.rotate_deg, b.rotate_deg, t);
        r.enabled = !r.is_identity();
        return format_transform(r);
    }

    // Colores: interpolación componente a componente en espacio sRGB premultiplicado
    // por alfa sólo cuando alguno de los extremos es translúcido.
    Rgba ca, cb;
    if (parse_rgba(f, ca) && parse_rgba(g, cb)) {
        Rgba out;
        out.a = lerp(ca.a, cb.a, t);
        if (ca.a >= 254.5f && cb.a >= 254.5f) {
            out.r = lerp(ca.r, cb.r, t);
            out.g = lerp(ca.g, cb.g, t);
            out.b = lerp(ca.b, cb.b, t);
        } else {
            // Premultiplicado: evita el "paso por gris" al animar hacia transparente.
            float pa_r = ca.r * (ca.a / 255.0f), pa_g = ca.g * (ca.a / 255.0f), pa_b = ca.b * (ca.a / 255.0f);
            float pb_r = cb.r * (cb.a / 255.0f), pb_g = cb.g * (cb.a / 255.0f), pb_b = cb.b * (cb.a / 255.0f);
            float pr = lerp(pa_r, pb_r, t), pg = lerp(pa_g, pb_g, t), pb = lerp(pa_b, pb_b, t);
            float inv = (out.a > 0.5f) ? (255.0f / out.a) : 0.0f;
            out.r = pr * inv;
            out.g = pg * inv;
            out.b = pb * inv;
        }
        return format_rgba(out);
    }

    // Longitudes y números sin unidad.
    float na = 0.0f, nb = 0.0f;
    std::string ua, ub;
    if (split_number_unit(f, na, ua) && split_number_unit(g, nb, ub)) {
        // Unidades distintas no son interpolables sin un contexto de layout:
        // CSS cae entonces al comportamiento discreto.
        if (ua == ub) {
            return fmt_num(lerp(na, nb, t)) + ua;
        }
        if (ua.empty() || ub.empty()) {
            return fmt_num(lerp(na, nb, t)) + (ua.empty() ? ub : ua);
        }
    }

    // Valor no interpolable: salto discreto en la mitad del recorrido.
    return (t < 0.5f) ? f : g;
}

// =============================================================================
// AnimationTimeline
// =============================================================================

std::map<std::string, std::string> AnimationTimeline::snapshot(const ComputedStyle& st) {
    std::map<std::string, std::string> out;
    auto px = [](float v) { return fmt_num(v) + "px"; };
    auto col = [](const Color& c) {
        Rgba r{static_cast<float>(c.r), static_cast<float>(c.g),
               static_cast<float>(c.b), static_cast<float>(c.a)};
        return format_rgba(r);
    };

    out["opacity"] = fmt_num(st.opacity);
    if (st.width >= 0.0f) out["width"] = px(st.width);
    if (st.height >= 0.0f) out["height"] = px(st.height);
    if (st.min_width >= 0.0f) out["min-width"] = px(st.min_width);
    if (st.max_width >= 0.0f) out["max-width"] = px(st.max_width);
    if (st.has_top) out["top"] = px(st.top);
    if (st.has_left) out["left"] = px(st.left);
    if (st.has_right) out["right"] = px(st.right);
    if (st.has_bottom) out["bottom"] = px(st.bottom);
    out["padding-top"] = px(st.padding.top);
    out["padding-right"] = px(st.padding.right);
    out["padding-bottom"] = px(st.padding.bottom);
    out["padding-left"] = px(st.padding.left);
    out["margin-top"] = px(st.margin.top);
    out["margin-right"] = px(st.margin.right);
    out["margin-bottom"] = px(st.margin.bottom);
    out["margin-left"] = px(st.margin.left);
    out["background"] = col(st.background);
    out["color"] = col(st.color);
    out["border-color"] = col(st.border_color);
    out["border-width"] = px(st.border_width);
    out["border-radius"] = px(st.border_radius);
    out["font-size"] = px(st.font_size);
    out["line-height"] = fmt_num(st.line_height);
    out["gap"] = px(st.gap);
    out["z-index"] = fmt_num(static_cast<float>(st.z_index));
    out["transform"] = format_transform(st.transform);
    return out;
}

AnimationSample AnimationTimeline::sample_keyframes(const AnimationSpec& spec,
                                                    double elapsed_ms) const {
    AnimationSample out;
    if (!sheet_ || !spec.valid()) return out;
    const CssKeyframesRule* rule = sheet_->find_keyframes(spec.name);
    if (!rule || rule->keyframes.empty()) return out;

    const double active_ms = elapsed_ms - static_cast<double>(spec.delay_ms);

    // Fase de retardo: sólo `backwards`/`both` muestran ya el primer fotograma.
    if (active_ms < 0.0) {
        if (spec.fill_mode == AnimationFillMode::Backwards ||
            spec.fill_mode == AnimationFillMode::Both) {
            out.declarations = rule->keyframes.front().declarations;
        }
        return out;
    }

    const double dur = static_cast<double>(spec.duration_ms);
    double raw_iteration = active_ms / dur;
    bool finished = false;
    if (!spec.infinite() && raw_iteration >= static_cast<double>(spec.iteration_count)) {
        raw_iteration = static_cast<double>(spec.iteration_count);
        finished = true;
    }

    int64_t iteration = static_cast<int64_t>(std::floor(raw_iteration));
    double local = raw_iteration - static_cast<double>(iteration);
    if (finished) {
        // Al terminar, el fotograma congelado es el final de la última iteración.
        if (local == 0.0 && iteration > 0) {
            --iteration;
            local = 1.0;
        }
    }

    float linear_t = static_cast<float>(std::clamp(local, 0.0, 1.0));

    // Dirección de reproducción.
    bool backwards = false;
    switch (spec.direction) {
        case AnimationDirection::Normal: backwards = false; break;
        case AnimationDirection::Reverse: backwards = true; break;
        case AnimationDirection::Alternate: backwards = (iteration % 2) != 0; break;
        case AnimationDirection::AlternateReverse: backwards = (iteration % 2) == 0; break;
    }
    if (backwards) linear_t = 1.0f - linear_t;

    const float eased = spec.easing.eval(linear_t);
    out.progress = eased;
    out.iteration = iteration;
    out.finished = finished;
    out.active = !finished ||
                 spec.fill_mode == AnimationFillMode::Forwards ||
                 spec.fill_mode == AnimationFillMode::Both;

    if (finished && !out.active) return out;

    // CSS Animations Level 1 §4: cada propiedad tiene su PROPIA lista de
    // keyframes, formada sólo por los pasos que la declaran. Así, en
    // `from{width;opacity} 50%{width} to{width;opacity}`, al 50% el ancho toma
    // el valor del paso intermedio mientras la opacidad sigue interpolando
    // entre `from` y `to`, que es lo que hace un navegador real.
    std::map<std::string, std::vector<std::pair<float, std::string>>> per_property;
    for (const auto& kf : rule->keyframes) {
        for (const auto& d : kf.declarations) {
            per_property[d.property].emplace_back(kf.offset, d.value);
        }
    }

    for (const auto& [prop, steps] : per_property) {
        if (steps.empty()) continue;
        // Antes del primer paso o después del último, el valor se mantiene
        // (equivale a tomar el valor subyacente del elemento como extremo).
        if (eased <= steps.front().first) {
            out.declarations.push_back(CssDeclaration{prop, steps.front().second});
            continue;
        }
        if (eased >= steps.back().first) {
            out.declarations.push_back(CssDeclaration{prop, steps.back().second});
            continue;
        }

        size_t hi_idx = 0;
        while (hi_idx < steps.size() && steps[hi_idx].first < eased) ++hi_idx;
        const size_t lo_idx = (hi_idx == 0) ? 0 : hi_idx - 1;

        const float lo_off = steps[lo_idx].first;
        const float hi_off = steps[hi_idx].first;
        const float span = hi_off - lo_off;
        const float local_t = (span > 1e-6f) ? std::clamp((eased - lo_off) / span, 0.0f, 1.0f) : 0.0f;

        out.declarations.push_back(CssDeclaration{
            prop, ValueInterpolator::interpolate(prop, steps[lo_idx].second,
                                                 steps[hi_idx].second, local_t)});
    }

    return out;
}

void AnimationTimeline::register_animation(const DomNode* node,
                                           const AnimationSpec& spec,
                                           double now_ms) {
    if (!node) return;

    if (!spec.valid()) {
        auto it = states_.find(node);
        if (it != states_.end()) {
            it->second.has_animation = false;
            it->second.animation = AnimationState{};
        }
        return;
    }

    NodeState& ns = states_[node];
    // Reiniciar sólo si cambia la identidad de la animación: así un relayout
    // (o un segundo compute_style en el mismo fotograma) no la rearranca.
    if (!ns.has_animation || ns.animation.spec.name != spec.name ||
        ns.animation.spec.duration_ms != spec.duration_ms ||
        ns.animation.spec.delay_ms != spec.delay_ms ||
        ns.animation.spec.iteration_count != spec.iteration_count ||
        ns.animation.spec.direction != spec.direction) {
        ns.animation = AnimationState{};
        ns.animation.spec = spec;
        ns.animation.start_ms = now_ms;
        ns.has_animation = true;
        auto sample = sample_keyframes(spec, 0.0);
        ns.animation.current = sample.declarations;
    } else {
        ns.animation.spec = spec; // easing / fill-mode pueden cambiar sin reiniciar
    }
}

void AnimationTimeline::register_transitions(const DomNode* node,
                                             const std::vector<TransitionSpec>& specs,
                                             const std::map<std::string, std::string>& resolved,
                                             double now_ms) {
    if (!node) return;
    if (specs.empty()) {
        auto it = states_.find(node);
        if (it != states_.end()) {
            it->second.transitions.clear();
            it->second.last_base_values = resolved;
        }
        return;
    }

    NodeState& ns = states_[node];

    for (const auto& [prop, new_value] : resolved) {
        if (!ValueInterpolator::is_animatable(prop)) continue;

        const TransitionSpec* spec = nullptr;
        for (const auto& ts : specs) {
            if (ts.matches(prop) && ts.duration_ms > 0.0f) {
                spec = &ts;
                // Una coincidencia exacta gana sobre `all`.
                if (ts.property == prop) break;
            }
        }
        if (!spec) continue;

        auto prev = ns.last_base_values.find(prop);
        if (prev == ns.last_base_values.end()) continue;      // primer cálculo: sin salto
        if (prev->second == new_value) continue;              // sin cambio: nada que animar

        // Si ya había una transición de esa propiedad, arranca desde su valor
        // actual (interrupción de transición, como exige CSS Transitions).
        std::string from = prev->second;
        for (const auto& running : ns.transitions) {
            if (running.property == prop && !running.finished && !running.current.empty()) {
                from = running.current;
                break;
            }
        }

        ns.transitions.erase(
            std::remove_if(ns.transitions.begin(), ns.transitions.end(),
                           [&](const TransitionState& t) { return t.property == prop; }),
            ns.transitions.end());

        TransitionState t;
        t.property = prop;
        t.from = from;
        t.to = new_value;
        t.start_ms = now_ms;
        t.duration_ms = spec->duration_ms;
        t.delay_ms = spec->delay_ms;
        t.easing = spec->easing;
        t.current = from;
        ns.transitions.push_back(std::move(t));
    }

    ns.last_base_values = resolved;
}

bool AnimationTimeline::advance(double now_ms) {
    clock_ms_ = now_ms;
    bool changed = false;

    for (auto& [node, ns] : states_) {
        if (ns.has_animation && ns.animation.spec.valid()) {
            auto sample = sample_keyframes(ns.animation.spec, now_ms - ns.animation.start_ms);
            if (sample.declarations != ns.animation.current) {
                ns.animation.current = sample.declarations;
                changed = true;
            }
            if (sample.finished && !ns.animation.finished) {
                ns.animation.finished = true;
                changed = true;
            }
        }

        for (auto& t : ns.transitions) {
            if (t.finished) continue;
            double active = now_ms - t.start_ms - static_cast<double>(t.delay_ms);
            float linear_t;
            if (active <= 0.0) {
                linear_t = 0.0f;
            } else if (active >= static_cast<double>(t.duration_ms)) {
                linear_t = 1.0f;
                t.finished = true;
            } else {
                linear_t = static_cast<float>(active / static_cast<double>(t.duration_ms));
            }
            std::string value =
                ValueInterpolator::interpolate(t.property, t.from, t.to, t.easing.eval(linear_t));
            if (value != t.current) {
                t.current = std::move(value);
                changed = true;
            }
        }
    }

    return changed;
}

void AnimationTimeline::collect(const DomNode* node, std::vector<CssDeclaration>& out) const {
    auto it = states_.find(node);
    if (it == states_.end()) return;
    const NodeState& ns = it->second;

    if (ns.has_animation) {
        for (const auto& d : ns.animation.current) out.push_back(d);
    }
    // Las transiciones se aplican después: una transición en curso sobre una
    // propiedad concreta gana al valor base, pero no a la animación declarada.
    for (const auto& t : ns.transitions) {
        if (t.current.empty()) continue;
        bool shadowed = false;
        if (ns.has_animation) {
            for (const auto& d : ns.animation.current) {
                if (d.property == t.property) { shadowed = true; break; }
            }
        }
        if (!shadowed) out.push_back(CssDeclaration{t.property, t.current});
    }
}

void AnimationTimeline::retain_only(const HtmlDocument& doc) {
    std::vector<const DomNode*> alive;
    std::vector<const DomNode*> stack;
    if (doc.root()) stack.push_back(doc.root());
    while (!stack.empty()) {
        const DomNode* n = stack.back();
        stack.pop_back();
        alive.push_back(n);
        for (const DomNode* child : n->children) {
            if (child) stack.push_back(child);
        }
    }

    for (auto it = states_.begin(); it != states_.end();) {
        if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

void AnimationTimeline::clear() {
    states_.clear();
    clock_ms_ = 0.0;
}

size_t AnimationTimeline::active_animation_count() const {
    size_t n = 0;
    for (const auto& [node, ns] : states_) {
        if (ns.has_animation && !ns.animation.current.empty()) ++n;
    }
    return n;
}

size_t AnimationTimeline::active_transition_count() const {
    size_t n = 0;
    for (const auto& [node, ns] : states_) {
        for (const auto& t : ns.transitions) {
            if (!t.finished) ++n;
        }
    }
    return n;
}

} // namespace astra
