#include "style/css_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

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

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<CssDeclaration> parse_declarations(std::string_view block) {
    std::vector<CssDeclaration> decls;
    size_t start = 0;
    while (start < block.size()) {
        size_t semi = block.find(';', start);
        std::string_view item = (semi == std::string_view::npos) ? block.substr(start) : block.substr(start, semi - start);
        size_t colon = item.find(':');
        if (colon != std::string_view::npos) {
            std::string raw_prop = trim(item.substr(0, colon));
            std::string prop = (raw_prop.rfind("--", 0) == 0) ? raw_prop : to_lower(raw_prop);
            std::string val = trim(item.substr(colon + 1));
            // Strip !important suffix
            size_t bang = val.find("!important");
            if (bang != std::string::npos) val = trim(val.substr(0, bang));
            if (!prop.empty() && !val.empty()) {
                decls.push_back(CssDeclaration{std::move(prop), std::move(val)});
            }
        }
        if (semi == std::string_view::npos) break;
        start = semi + 1;
    }
    return decls;
}

// -----------------------------------------------------------------------------
// Session 8: utilidades de parseo para transform / animation / transition
// -----------------------------------------------------------------------------

// Divide "translate(4px, 8px) rotate(30deg)" en pares (función, argumentos),
// respetando los paréntesis anidados.
std::vector<std::pair<std::string, std::string>> split_functions(std::string_view v) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && (std::isspace(static_cast<unsigned char>(v[i])) || v[i] == ',')) ++i;
        size_t name_start = i;
        while (i < v.size() && v[i] != '(' && !std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        if (name_start == i) break;
        std::string fn = to_lower(v.substr(name_start, i - name_start));
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        if (i >= v.size() || v[i] != '(') {
            out.emplace_back(std::move(fn), std::string());
            continue;
        }
        ++i; // consume '('
        int depth = 1;
        size_t arg_start = i;
        while (i < v.size() && depth > 0) {
            if (v[i] == '(') ++depth;
            else if (v[i] == ')') --depth;
            if (depth > 0) ++i;
        }
        out.emplace_back(std::move(fn), trim(v.substr(arg_start, i - arg_start)));
        if (i < v.size()) ++i; // consume ')'
    }
    return out;
}

// Divide por comas en el nivel superior ("a, b(1, 2), c" -> 3 piezas).
std::vector<std::string> split_top_level_commas(std::string_view v) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '(') ++depth;
        else if (v[i] == ')') { if (depth > 0) --depth; }
        else if (v[i] == ',' && depth == 0) {
            std::string piece = trim(v.substr(start, i - start));
            if (!piece.empty()) out.push_back(std::move(piece));
            start = i + 1;
        }
    }
    std::string last = trim(v.substr(start));
    if (!last.empty()) out.push_back(std::move(last));
    return out;
}

bool is_time_token(std::string_view t) {
    if (t.size() < 2) return false;
    bool has_digit = false;
    for (char c : t) {
        if (std::isdigit(static_cast<unsigned char>(c))) has_digit = true;
    }
    if (!has_digit) return false;
    return (t.size() > 2 && to_lower(t.substr(t.size() - 2)) == "ms") ||
           (to_lower(t).back() == 's');
}

bool is_easing_token(std::string_view t) {
    std::string l = to_lower(t);
    return l == "linear" || l == "ease" || l == "ease-in" || l == "ease-out" ||
           l == "ease-in-out" || l == "step-start" || l == "step-end" ||
           l.rfind("cubic-bezier", 0) == 0 || l.rfind("steps", 0) == 0;
}

bool match_compound_selector(const DomNode* node, std::string_view part, int& specificity) {
    if (!node || node->type != NodeType::Element || part.empty()) return false;
    if (part == "*") return true;
    if (to_lower(part) == ":root") {
        if (node->tag == "html" || node->tag == "body") {
            specificity += 10;
            return true;
        }
        return false;
    }

    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    bool has_root_pseudo = false;

    size_t i = 0;
    while (i < part.size()) {
        if (part[i] == '#') {
            ++i;
            size_t st = i;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':') ++i;
            id = std::string(part.substr(st, i - st));
        } else if (part[i] == '.') {
            ++i;
            size_t st = i;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':') ++i;
            classes.emplace_back(part.substr(st, i - st));
        } else if (part[i] == ':') {
            size_t st = i;
            while (i < part.size() && part[i] != '.' && part[i] != '#') ++i;
            std::string pseudo = to_lower(part.substr(st, i - st));
            if (pseudo == ":root") has_root_pseudo = true;
        } else {
            size_t st = i;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':') ++i;
            tag = to_lower(part.substr(st, i - st));
        }
    }

    if (has_root_pseudo && node->tag != "html" && node->tag != "body") return false;
    if (!tag.empty() && node->tag != tag) return false;
    if (!id.empty() && node->id() != id) return false;
    for (const auto& c : classes) {
        if (!node->has_class(c)) return false;
    }

    if (!id.empty()) specificity += 100;
    specificity += static_cast<int>(classes.size()) * 10;
    if (has_root_pseudo) specificity += 10;
    if (!tag.empty()) specificity += 1;
    return true;
}

bool match_selector(const DomNode* node, std::string_view selector, int& specificity) {
    // Tokenize selector supporting both descendant (' ') and direct-child ('>') combinators
    std::vector<std::string> parts;
    std::vector<bool> direct_child; // direct_child[i] applies between parts[i] and parts[i+1]
    size_t i = 0;
    bool pending_child = false;
    while (i < selector.size()) {
        while (i < selector.size() && std::isspace(static_cast<unsigned char>(selector[i]))) ++i;
        if (i >= selector.size()) break;
        if (selector[i] == '>') {
            pending_child = true;
            ++i;
            continue;
        }
        size_t st = i;
        while (i < selector.size() && !std::isspace(static_cast<unsigned char>(selector[i])) && selector[i] != '>') {
            ++i;
        }
        if (i > st) {
            if (!parts.empty()) direct_child.push_back(pending_child);
            parts.emplace_back(selector.substr(st, i - st));
            pending_child = false;
        }
    }
    if (parts.empty()) return false;
    specificity = 0;
    if (!match_compound_selector(node, parts.back(), specificity)) return false;

    const DomNode* cur = node;
    for (int idx = static_cast<int>(parts.size()) - 2; idx >= 0; --idx) {
        bool is_direct = direct_child[static_cast<size_t>(idx)];
        if (is_direct) {
            const DomNode* p = cur ? cur->parent : nullptr;
            if (!p || p->type != NodeType::Element) return false;
            int sub_spec = 0;
            if (!match_compound_selector(p, parts[static_cast<size_t>(idx)], sub_spec)) return false;
            specificity += sub_spec;
            cur = p;
        } else {
            const DomNode* ancestor = cur ? cur->parent : nullptr;
            bool found = false;
            while (ancestor && ancestor->type == NodeType::Element) {
                int sub_spec = 0;
                if (match_compound_selector(ancestor, parts[static_cast<size_t>(idx)], sub_spec)) {
                    specificity += sub_spec;
                    found = true;
                    cur = ancestor;
                    break;
                }
                ancestor = ancestor->parent;
            }
            if (!found) return false;
        }
    }
    return true;
}

EdgeSizes parse_edges(std::string_view val) {
    auto toks = split_ws(val);
    EdgeSizes e;
    if (toks.size() == 1) {
        float v = StyleEngine::parse_px(toks[0]);
        e = {v, v, v, v};
    } else if (toks.size() == 2) {
        float v = StyleEngine::parse_px(toks[0]);
        float h = StyleEngine::parse_px(toks[1]);
        e = {v, h, v, h};
    } else if (toks.size() == 3) {
        e = {StyleEngine::parse_px(toks[0]), StyleEngine::parse_px(toks[1]),
             StyleEngine::parse_px(toks[2]), StyleEngine::parse_px(toks[1])};
    } else if (toks.size() >= 4) {
        e = {StyleEngine::parse_px(toks[0]), StyleEngine::parse_px(toks[1]),
             StyleEngine::parse_px(toks[2]), StyleEngine::parse_px(toks[3])};
    }
    return e;
}

std::vector<GridTrack> parse_grid_tracks(std::string_view val) {
    std::vector<GridTrack> tracks;
    std::string s = to_lower(trim(val));
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) break;
        if (s.compare(pos, 7, "repeat(") == 0) {
            size_t rp = s.find(')', pos + 7);
            if (rp == std::string::npos) break;
            std::string inner = s.substr(pos + 7, rp - (pos + 7));
            size_t comma = inner.find(',');
            if (comma != std::string::npos) {
                int count = std::clamp(std::atoi(inner.substr(0, comma).c_str()), 1, 16);
                std::string unit = trim(inner.substr(comma + 1));
                GridTrack tr;
                if (unit.find("fr") != std::string::npos) {
                    tr.is_fr = true;
                    tr.value = std::max(0.1f, std::strtof(unit.c_str(), nullptr));
                } else {
                    tr.is_fr = false;
                    tr.value = std::max(24.0f, StyleEngine::parse_px(unit, 120.0f));
                }
                for (int i = 0; i < count; ++i) tracks.push_back(tr);
            }
            pos = rp + 1;
        } else {
            size_t st = pos;
            while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            std::string tok = s.substr(st, pos - st);
            GridTrack tr;
            if (tok.find("fr") != std::string::npos) {
                tr.is_fr = true;
                tr.value = std::max(0.1f, std::strtof(tok.c_str(), nullptr));
            } else if (tok == "auto") {
                tr.is_fr = true;
                tr.value = 1.0f;
            } else {
                tr.is_fr = false;
                tr.value = std::max(24.0f, StyleEngine::parse_px(tok, 120.0f));
            }
            tracks.push_back(tr);
        }
    }
    return tracks;
}

void apply_declaration(ComputedStyle& st, const CssDeclaration& d) {
    const std::string& p = d.property;
    const std::string& v = d.value;
    std::string vl = to_lower(v);

    if (p == "display") {
        if (vl == "none") st.display = DisplayType::None;
        else if (vl == "flex") st.display = DisplayType::Flex;
        else if (vl == "grid") st.display = DisplayType::Grid;
        else if (vl == "inline" || vl == "inline-block") st.display = DisplayType::Inline;
        else st.display = DisplayType::Block;
    } else if (p == "flex-direction") {
        st.flex_direction = (vl == "column") ? FlexDirection::Column : FlexDirection::Row;
    } else if (p == "justify-content") {
        if (vl == "center") st.justify_content = JustifyContent::Center;
        else if (vl == "flex-end" || vl == "end" || vl == "right") st.justify_content = JustifyContent::FlexEnd;
        else if (vl == "space-between") st.justify_content = JustifyContent::SpaceBetween;
        else st.justify_content = JustifyContent::FlexStart;
    } else if (p == "align-items") {
        if (vl == "center") st.align_items = AlignItems::Center;
        else if (vl == "flex-start" || vl == "start") st.align_items = AlignItems::FlexStart;
        else if (vl == "flex-end" || vl == "end") st.align_items = AlignItems::FlexEnd;
        else st.align_items = AlignItems::Stretch;
    } else if (p == "text-align") {
        if (vl == "center") st.text_align = TextAlign::Center;
        else if (vl == "right" || vl == "end") st.text_align = TextAlign::Right;
        else st.text_align = TextAlign::Left;
    } else if (p == "grid-template-columns") {
        st.grid_columns = parse_grid_tracks(v);
    } else if (p == "gap" || p == "grid-gap") {
        st.gap = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "width") {
        if (vl != "auto" && vl.find('%') == std::string::npos) st.width = StyleEngine::parse_px(v, -1.0f);
    } else if (p == "min-width") {
        if (vl != "auto" && vl.find('%') == std::string::npos) st.min_width = StyleEngine::parse_px(v, -1.0f);
    } else if (p == "max-width") {
        if (vl != "none" && vl.find('%') == std::string::npos) st.max_width = StyleEngine::parse_px(v, -1.0f);
    } else if (p == "height") {
        if (vl != "auto" && vl.find('%') == std::string::npos) st.height = StyleEngine::parse_px(v, -1.0f);
    } else if (p == "margin") {
        st.margin = parse_edges(v);
    } else if (p == "margin-top") {
        st.margin.top = StyleEngine::parse_px(v);
    } else if (p == "margin-bottom") {
        st.margin.bottom = StyleEngine::parse_px(v);
    } else if (p == "margin-left") {
        st.margin.left = StyleEngine::parse_px(v);
    } else if (p == "margin-right") {
        st.margin.right = StyleEngine::parse_px(v);
    } else if (p == "padding") {
        st.padding = parse_edges(v);
    } else if (p == "padding-top") {
        st.padding.top = StyleEngine::parse_px(v);
    } else if (p == "padding-bottom") {
        st.padding.bottom = StyleEngine::parse_px(v);
    } else if (p == "padding-left") {
        st.padding.left = StyleEngine::parse_px(v);
    } else if (p == "padding-right") {
        st.padding.right = StyleEngine::parse_px(v);
    } else if (p == "background" || p == "background-color" || p == "background-image") {
        size_t lg = vl.find("linear-gradient(");
        if (lg != std::string::npos) {
            size_t start = lg + 16;
            int depth = 1;
            size_t end = start;
            while (end < v.size() && depth > 0) {
                if (v[end] == '(') ++depth;
                else if (v[end] == ')') --depth;
                if (depth > 0) ++end;
            }
            std::string inner = v.substr(start, end - start);
            // Split top-level comma-separated args (preserving rgb(...) / rgba(...) commas)
            std::vector<std::string> args;
            int pd = 0;
            size_t seg = 0;
            for (size_t i = 0; i <= inner.size(); ++i) {
                if (i < inner.size()) {
                    if (inner[i] == '(') ++pd;
                    else if (inner[i] == ')') --pd;
                }
                if (i == inner.size() || (inner[i] == ',' && pd == 0)) {
                    std::string tok = trim(inner.substr(seg, i - seg));
                    if (!tok.empty()) args.push_back(std::move(tok));
                    seg = i + 1;
                }
            }
            if (args.size() >= 2) {
                float angle = 180.0f;
                size_t color_idx = 0;
                std::string a0 = to_lower(args[0]);
                if (a0.find("deg") != std::string::npos) {
                    angle = std::strtof(a0.c_str(), nullptr);
                    color_idx = 1;
                } else if (a0.rfind("to ", 0) == 0) {
                    if (a0.find("right") != std::string::npos && a0.find("bottom") != std::string::npos) angle = 135.0f;
                    else if (a0.find("right") != std::string::npos && a0.find("top") != std::string::npos) angle = 45.0f;
                    else if (a0.find("left") != std::string::npos && a0.find("bottom") != std::string::npos) angle = 225.0f;
                    else if (a0.find("left") != std::string::npos && a0.find("top") != std::string::npos) angle = 315.0f;
                    else if (a0.find("right") != std::string::npos) angle = 90.0f;
                    else if (a0.find("left") != std::string::npos) angle = 270.0f;
                    else if (a0.find("top") != std::string::npos) angle = 0.0f;
                    else angle = 180.0f;
                    color_idx = 1;
                }
                if (color_idx + 1 < args.size()) {
                    auto strip_stop = [](const std::string& s) {
                        if (s.find('(') != std::string::npos) {
                            size_t rp = s.find(')');
                            return rp != std::string::npos ? s.substr(0, rp + 1) : s;
                        }
                        auto parts = split_ws(s);
                        return parts.empty() ? s : parts[0];
                    };
                    st.background_gradient.enabled = true;
                    st.background_gradient.angle_deg = angle;
                    st.background_gradient.start = StyleEngine::parse_color(strip_stop(args[color_idx]), Color{15, 23, 42, 255});
                    st.background_gradient.end = StyleEngine::parse_color(strip_stop(args.back()), Color{30, 58, 138, 255});
                }
            }
        } else if (p != "background-image") {
            st.background_gradient.enabled = false;
            auto toks = split_ws(v);
            if (!toks.empty()) st.background = StyleEngine::parse_color(toks[0], st.background);
        }
    } else if (p == "box-sizing") {
        st.box_sizing = (vl == "border-box") ? BoxSizing::BorderBox : BoxSizing::ContentBox;
    } else if (p == "color") {
        st.color = StyleEngine::parse_color(v, st.color);
    } else if (p == "border" || p == "border-left" || p == "border-top" || p == "border-right" || p == "border-bottom") {
        auto toks = split_ws(v);
        for (const auto& t : toks) {
            if (std::isdigit(static_cast<unsigned char>(t[0]))) {
                st.border_width = StyleEngine::parse_px(t, 1.0f);
            } else if (t != "solid" && t != "dashed" && t != "dotted" && t != "none") {
                st.border_color = StyleEngine::parse_color(t, Color{80, 90, 110, 255});
            }
        }
    } else if (p == "border-color") {
        st.border_color = StyleEngine::parse_color(v, st.border_color);
    } else if (p == "border-width") {
        st.border_width = StyleEngine::parse_px(v, 1.0f);
    } else if (p == "border-radius") {
        st.border_radius = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "transform") {
        st.transform = StyleEngine::parse_transform(v);
    } else if (p == "animation") {
        // Atajo de orden libre: el primer tiempo es la duración, el segundo el
        // retardo; el resto se identifica por su forma.
        AnimationSpec spec;
        bool got_duration = false;
        for (const auto& raw : split_ws(v)) {
            std::string t = to_lower(raw);
            if (is_time_token(t)) {
                if (!got_duration) { spec.duration_ms = StyleEngine::parse_time_ms(t, 0.0f); got_duration = true; }
                else spec.delay_ms = StyleEngine::parse_time_ms(t, 0.0f);
            } else if (is_easing_token(t)) {
                spec.easing = Easing::parse(t, spec.easing);
            } else if (t == "infinite") {
                spec.iteration_count = -1.0f;
            } else if (t == "normal") {
                spec.direction = AnimationDirection::Normal;
            } else if (t == "reverse") {
                spec.direction = AnimationDirection::Reverse;
            } else if (t == "alternate") {
                spec.direction = AnimationDirection::Alternate;
            } else if (t == "alternate-reverse") {
                spec.direction = AnimationDirection::AlternateReverse;
            } else if (t == "forwards") {
                spec.fill_mode = (spec.fill_mode == AnimationFillMode::Backwards)
                                     ? AnimationFillMode::Both : AnimationFillMode::Forwards;
            } else if (t == "backwards") {
                spec.fill_mode = (spec.fill_mode == AnimationFillMode::Forwards)
                                     ? AnimationFillMode::Both : AnimationFillMode::Backwards;
            } else if (t == "both") {
                spec.fill_mode = AnimationFillMode::Both;
            } else if (t == "none" || t == "running" || t == "paused") {
                // Ignorado: no hay control de reproducción por script todavía.
            } else if (!t.empty() && std::isdigit(static_cast<unsigned char>(t[0]))) {
                spec.iteration_count = static_cast<float>(std::atof(t.c_str()));
            } else if (spec.name.empty()) {
                spec.name = std::string(raw);
            }
        }
        st.animation = spec;
    } else if (p == "animation-name") {
        st.animation.name = (vl == "none") ? std::string() : trim(v);
    } else if (p == "animation-duration") {
        st.animation.duration_ms = StyleEngine::parse_time_ms(v, 0.0f);
    } else if (p == "animation-delay") {
        st.animation.delay_ms = StyleEngine::parse_time_ms(v, 0.0f);
    } else if (p == "animation-timing-function") {
        st.animation.easing = Easing::parse(v, st.animation.easing);
    } else if (p == "animation-iteration-count") {
        st.animation.iteration_count = (vl == "infinite") ? -1.0f : static_cast<float>(std::atof(vl.c_str()));
    } else if (p == "animation-direction") {
        if (vl == "reverse") st.animation.direction = AnimationDirection::Reverse;
        else if (vl == "alternate") st.animation.direction = AnimationDirection::Alternate;
        else if (vl == "alternate-reverse") st.animation.direction = AnimationDirection::AlternateReverse;
        else st.animation.direction = AnimationDirection::Normal;
    } else if (p == "animation-fill-mode") {
        if (vl == "forwards") st.animation.fill_mode = AnimationFillMode::Forwards;
        else if (vl == "backwards") st.animation.fill_mode = AnimationFillMode::Backwards;
        else if (vl == "both") st.animation.fill_mode = AnimationFillMode::Both;
        else st.animation.fill_mode = AnimationFillMode::None;
    } else if (p == "transition") {
        st.transitions.clear();
        if (vl != "none") {
            for (const auto& entry : split_top_level_commas(v)) {
                TransitionSpec ts;
                bool got_duration = false;
                bool got_property = false;
                for (const auto& raw : split_ws(entry)) {
                    std::string t = to_lower(raw);
                    if (is_time_token(t)) {
                        if (!got_duration) { ts.duration_ms = StyleEngine::parse_time_ms(t, 0.0f); got_duration = true; }
                        else ts.delay_ms = StyleEngine::parse_time_ms(t, 0.0f);
                    } else if (is_easing_token(t)) {
                        ts.easing = Easing::parse(t, ts.easing);
                    } else if (!got_property) {
                        ts.property = t;
                        got_property = true;
                    }
                }
                if (ts.duration_ms > 0.0f) st.transitions.push_back(std::move(ts));
            }
        }
    } else if (p == "transition-property") {
        if (st.transitions.empty()) st.transitions.emplace_back();
        for (auto& ts : st.transitions) ts.property = vl;
    } else if (p == "transition-duration") {
        if (st.transitions.empty()) st.transitions.emplace_back();
        for (auto& ts : st.transitions) ts.duration_ms = StyleEngine::parse_time_ms(v, 0.0f);
    } else if (p == "transition-delay") {
        if (st.transitions.empty()) st.transitions.emplace_back();
        for (auto& ts : st.transitions) ts.delay_ms = StyleEngine::parse_time_ms(v, 0.0f);
    } else if (p == "transition-timing-function") {
        if (st.transitions.empty()) st.transitions.emplace_back();
        for (auto& ts : st.transitions) ts.easing = Easing::parse(v, ts.easing);
    } else if (p == "font-size") {
        st.font_size = std::clamp(StyleEngine::parse_px(v, st.font_size), 8.0f, 64.0f);
    } else if (p == "font-weight") {
        st.bold = (vl == "bold" || vl == "600" || vl == "700" || vl == "800" || vl == "900");
    } else if (p == "font-family") {
        st.monospace = (vl.find("mono") != std::string::npos || vl.find("code") != std::string::npos ||
                        vl.find("consolas") != std::string::npos || vl.find("courier") != std::string::npos);
    } else if (p == "position") {
        if (vl == "relative") st.position = PositionType::Relative;
        else if (vl == "absolute") st.position = PositionType::Absolute;
        else if (vl == "fixed") st.position = PositionType::Fixed;
        else st.position = PositionType::Static;
    } else if (p == "top") {
        st.has_top = true;
        st.top = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "left") {
        st.has_left = true;
        st.left = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "right") {
        st.has_right = true;
        st.right = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "bottom") {
        st.has_bottom = true;
        st.bottom = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "z-index") {
        st.z_index = std::atoi(v.c_str());
    } else if (p == "opacity") {
        st.opacity = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
    } else if (p == "overflow" || p == "overflow-x" || p == "overflow-y") {
        if (vl == "hidden" || vl == "clip" || vl == "scroll" || vl == "auto") {
            st.overflow = OverflowMode::Hidden;
        } else {
            st.overflow = OverflowMode::Visible;
        }
    } else if (p == "box-shadow") {
        if (vl == "none") {
            st.box_shadow.enabled = false;
        } else {
            st.box_shadow.enabled = true;
            // Extract optional rgba(...) or #hex color
            size_t rgba_pos = vl.find("rgba(");
            if (rgba_pos == std::string::npos) rgba_pos = vl.find("rgb(");
            std::string num_part = v;
            if (rgba_pos != std::string::npos) {
                size_t rp = vl.find(')', rgba_pos);
                if (rp != std::string::npos) {
                    st.box_shadow.color = StyleEngine::parse_color(v.substr(rgba_pos, rp - rgba_pos + 1), Color{0, 0, 0, 120});
                    num_part = v.substr(0, rgba_pos);
                }
            }
            auto toks = split_ws(num_part);
            std::vector<float> nums;
            for (const auto& t : toks) {
                if (!t.empty() && (t[0] == '#' || std::isalpha(static_cast<unsigned char>(t[0])))) {
                    st.box_shadow.color = StyleEngine::parse_color(t, st.box_shadow.color);
                } else {
                    nums.push_back(StyleEngine::parse_px(t, 0.0f));
                }
            }
            if (nums.size() >= 1) st.box_shadow.offset_x = nums[0];
            if (nums.size() >= 2) st.box_shadow.offset_y = nums[1];
            if (nums.size() >= 3) st.box_shadow.blur = std::clamp(nums[2], 0.0f, 40.0f);
        }
    }
}

void apply_ua_defaults(const DomNode* node, ComputedStyle& st) {
    if (!node || node->type != NodeType::Element) return;
    const std::string& t = node->tag;
    if (t == "head" || t == "style" || t == "script" || t == "meta" || t == "link" || t == "title") {
        st.display = DisplayType::None;
    } else if (t == "span" || t == "a" || t == "strong" || t == "b" || t == "em" || t == "code") {
        st.display = DisplayType::Inline;
        if (t == "a") st.color = Color{96, 175, 255, 255};
        if (t == "strong" || t == "b") st.bold = true;
        if (t == "code") {
            st.background = Color{28, 34, 46, 255};
            st.color = Color{135, 232, 182, 255};
            st.padding = {2, 5, 2, 5};
            st.monospace = true;
        }
    } else if (t == "body") {
        st.background = Color{13, 17, 24, 255};
        st.color = Color{226, 232, 240, 255};
        st.padding = {24, 28, 28, 28};
        st.font_size = 15.0f;
    } else if (t == "h1") {
        st.font_size = 28.0f;
        st.bold = true;
        st.margin = {8, 0, 14, 0};
        st.color = Color{248, 250, 252, 255};
    } else if (t == "h2") {
        st.font_size = 22.0f;
        st.bold = true;
        st.margin = {14, 0, 10, 0};
        st.color = Color{241, 245, 249, 255};
    } else if (t == "h3") {
        st.font_size = 17.0f;
        st.bold = true;
        st.margin = {10, 0, 8, 0};
    } else if (t == "p") {
        st.margin = {0, 0, 10, 0};
    } else if (t == "pre") {
        st.background = Color{18, 23, 33, 255};
        st.color = Color{166, 226, 194, 255};
        st.padding = {12, 14, 12, 14};
        st.border_radius = 8.0f;
        st.border_width = 1.0f;
        st.border_color = Color{45, 56, 76, 255};
        st.margin = {6, 0, 12, 0};
        st.monospace = true;
    } else if (t == "input" || t == "textarea") {
        st.background = Color{15, 23, 42, 255};
        st.color = Color{241, 245, 249, 255};
        st.border_width = 1.0f;
        st.border_color = Color{51, 65, 85, 255};
        st.border_radius = 6.0f;
        st.padding = {8, 12, 8, 12};
        st.font_size = 14.0f;
        st.height = (t == "textarea") ? 88.0f : 38.0f;
    } else if (t == "ul" || t == "ol") {
        st.padding = {0, 0, 0, 20};
        st.margin = {4, 0, 10, 0};
    } else if (t == "li") {
        st.margin = {0, 0, 5, 0};
    } else if (t == "blockquote") {
        st.padding = {10, 14, 10, 16};
        st.margin = {8, 0, 12, 0};
        st.background = Color{22, 29, 42, 255};
        st.border_width = 2.0f;
        st.border_color = Color{56, 189, 248, 255};
        st.border_radius = 6.0f;
    } else if (t == "hr") {
        st.height = 1.0f;
        st.background = Color{51, 65, 85, 255};
        st.margin = {12, 0, 12, 0};
    }
}

} // namespace

void CssStylesheet::parse(std::string_view css) {
    parse_block(css, -1.0f, -1.0f);
}

void CssStylesheet::parse_block(std::string_view css, float min_w, float max_w) {
    size_t pos = 0;
    const size_t n = css.size();

    while (pos < n) {
        // Skip comments and whitespace
        while (pos < n) {
            if (std::isspace(static_cast<unsigned char>(css[pos]))) { ++pos; continue; }
            if (pos + 1 < n && css[pos] == '/' && css[pos + 1] == '*') {
                size_t end = css.find("*/", pos + 2);
                pos = (end == std::string_view::npos) ? n : end + 2;
                continue;
            }
            break;
        }
        if (pos >= n) break;

        // Parse @media blocks; skip other @rules cleanly
        if (css[pos] == '@') {
            size_t brace = css.find('{', pos);
            size_t semi = css.find(';', pos);
            if (semi != std::string_view::npos && (brace == std::string_view::npos || semi < brace)) {
                pos = semi + 1;
                continue;
            }
            if (brace != std::string_view::npos) {
                std::string header = to_lower(trim(css.substr(pos, brace - pos)));
                int depth = 1;
                size_t inner_start = brace + 1;
                pos = brace + 1;
                while (pos < n && depth > 0) {
                    if (css[pos] == '{') ++depth;
                    else if (css[pos] == '}') --depth;
                    ++pos;
                }
                size_t inner_end = (pos > 0 && depth == 0) ? (pos - 1) : pos;
                if ((header.rfind("@keyframes", 0) == 0 ||
                     header.rfind("@-webkit-keyframes", 0) == 0 ||
                     header.rfind("@-moz-keyframes", 0) == 0) && inner_end > inner_start) {
                    // Normaliza el prefijo de proveedor al nombre canónico.
                    std::string h = header;
                    size_t kw = h.find("keyframes");
                    if (kw != std::string::npos) h = "@keyframes" + h.substr(kw + 9);
                    parse_keyframes_block(h, css.substr(inner_start, inner_end - inner_start));
                    continue;
                }
                if (header.rfind("@media", 0) == 0 && inner_end > inner_start) {
                    float q_min = min_w;
                    float q_max = max_w;
                    size_t pmax = header.find("max-width");
                    if (pmax != std::string::npos) {
                        size_t col = header.find(':', pmax);
                        if (col != std::string::npos) {
                            float v = StyleEngine::parse_px(header.substr(col + 1), -1.0f);
                            if (v > 0.0f) q_max = (q_max < 0.0f) ? v : std::min(q_max, v);
                        }
                    }
                    size_t pmin = header.find("min-width");
                    if (pmin != std::string::npos) {
                        size_t col = header.find(':', pmin);
                        if (col != std::string::npos) {
                            float v = StyleEngine::parse_px(header.substr(col + 1), -1.0f);
                            if (v > 0.0f) q_min = (q_min < 0.0f) ? v : std::max(q_min, v);
                        }
                    }
                    parse_block(css.substr(inner_start, inner_end - inner_start), q_min, q_max);
                }
                continue;
            }
            break;
        }

        size_t lbrace = css.find('{', pos);
        if (lbrace == std::string_view::npos) break;
        std::string_view sel_group = css.substr(pos, lbrace - pos);
        size_t rbrace = css.find('}', lbrace + 1);
        if (rbrace == std::string_view::npos) break;
        std::string_view body = css.substr(lbrace + 1, rbrace - lbrace - 1);
        pos = rbrace + 1;

        CssRule rule;
        rule.source_order = rules_.size();
        rule.media_min_width = min_w;
        rule.media_max_width = max_w;
        size_t s = 0;
        while (s < sel_group.size()) {
            size_t comma = sel_group.find(',', s);
            std::string_view part = (comma == std::string_view::npos) ? sel_group.substr(s) : sel_group.substr(s, comma - s);
            std::string t = trim(part);
            if (!t.empty()) rule.selectors.push_back(std::move(t));
            if (comma == std::string_view::npos) break;
            s = comma + 1;
        }
        rule.declarations = parse_declarations(body);
        if (!rule.selectors.empty() && !rule.declarations.empty()) {
            rules_.push_back(std::move(rule));
        }
    }
}

Color StyleEngine::parse_color(std::string_view val, Color fallback) {
    std::string s = to_lower(trim(val));
    if (s.empty()) return fallback;
    if (s == "transparent") return Color{0, 0, 0, 0};

    static const std::unordered_map<std::string, Color> kNamed = {
        {"black", {0, 0, 0, 255}}, {"white", {255, 255, 255, 255}},
        {"red", {239, 68, 68, 255}}, {"green", {34, 197, 94, 255}},
        {"blue", {59, 130, 246, 255}}, {"cyan", {6, 182, 212, 255}},
        {"yellow", {234, 179, 8, 255}}, {"orange", {249, 115, 22, 255}},
        {"gray", {148, 163, 184, 255}}, {"slate", {100, 116, 139, 255}},
        {"emerald", {16, 185, 129, 255}}, {"sky", {56, 189, 248, 255}}
    };
    auto it = kNamed.find(s);
    if (it != kNamed.end()) return it->second;

    if (s[0] == '#') {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            return 0;
        };
        if (s.size() == 4) {
            return Color{
                static_cast<uint8_t>(hex(s[1]) * 17),
                static_cast<uint8_t>(hex(s[2]) * 17),
                static_cast<uint8_t>(hex(s[3]) * 17),
                255
            };
        }
        if (s.size() == 7 || s.size() == 9) {
            uint8_t r = static_cast<uint8_t>((hex(s[1]) << 4) | hex(s[2]));
            uint8_t g = static_cast<uint8_t>((hex(s[3]) << 4) | hex(s[4]));
            uint8_t b = static_cast<uint8_t>((hex(s[5]) << 4) | hex(s[6]));
            uint8_t a = (s.size() == 9) ? static_cast<uint8_t>((hex(s[7]) << 4) | hex(s[8])) : 255;
            return Color{r, g, b, a};
        }
    }

    if (s.rfind("rgb(", 0) == 0 || s.rfind("rgba(", 0) == 0) {
        size_t lp = s.find('(');
        size_t rp = s.find(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
            std::string inner = s.substr(lp + 1, rp - lp - 1);
            std::replace(inner.begin(), inner.end(), ',', ' ');
            std::istringstream iss(inner);
            float r = 0, g = 0, b = 0, a = 1.0f;
            if (iss >> r >> g >> b) {
                if (!(iss >> a)) a = 1.0f;
                return Color{
                    static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f))
                };
            }
        }
    }
    return fallback;
}

float StyleEngine::parse_px(std::string_view val, float fallback) {
    std::string s = trim(val);
    if (s.empty()) return fallback;
    char* end = nullptr;
    float num = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return fallback;
    if (s.find("rem") != std::string::npos || s.find("em") != std::string::npos) {
        return num * 16.0f;
    }
    return num;
}

std::string StyleEngine::resolve_var_expr(std::string_view val,
                                          const std::unordered_map<std::string, std::string>& vars,
                                          int depth) {
    if (depth > 8 || val.find("var(") == std::string_view::npos) {
        return std::string(val);
    }
    std::string out;
    out.reserve(val.size());
    size_t pos = 0;
    while (pos < val.size()) {
        size_t vp = val.find("var(", pos);
        if (vp == std::string_view::npos) {
            out.append(val.substr(pos));
            break;
        }
        out.append(val.substr(pos, vp - pos));
        size_t inner_start = vp + 4;
        int pdepth = 1;
        size_t cur = inner_start;
        while (cur < val.size() && pdepth > 0) {
            if (val[cur] == '(') ++pdepth;
            else if (val[cur] == ')') --pdepth;
            if (pdepth > 0) ++cur;
        }
        if (pdepth != 0) {
            out.append(val.substr(vp));
            break;
        }
        std::string_view inner = val.substr(inner_start, cur - inner_start);
        // Split at top-level comma for optional fallback
        int cdepth = 0;
        size_t comma = std::string_view::npos;
        for (size_t k = 0; k < inner.size(); ++k) {
            if (inner[k] == '(') ++cdepth;
            else if (inner[k] == ')') --cdepth;
            else if (inner[k] == ',' && cdepth == 0) { comma = k; break; }
        }
        std::string var_name = trim(comma == std::string_view::npos ? inner : inner.substr(0, comma));
        std::string fallback = (comma == std::string_view::npos) ? "" : trim(inner.substr(comma + 1));

        auto it = vars.find(var_name);
        if (it != vars.end() && !it->second.empty()) {
            out.append(resolve_var_expr(it->second, vars, depth + 1));
        } else if (!fallback.empty()) {
            out.append(resolve_var_expr(fallback, vars, depth + 1));
        }
        pos = cur + 1;
    }
    return out;
}

std::unordered_map<std::string, std::string> StyleEngine::collect_root_vars(const CssStylesheet& sheet,
                                                                            float viewport_w) {
    std::unordered_map<std::string, std::string> vars;
    for (const auto& rule : sheet.rules()) {
        if (rule.media_min_width >= 0.0f && viewport_w < rule.media_min_width) continue;
        if (rule.media_max_width >= 0.0f && viewport_w > rule.media_max_width) continue;
        bool is_root = false;
        for (const auto& sel : rule.selectors) {
            std::string sl = to_lower(trim(sel));
            if (sl == ":root" || sl == "html") {
                is_root = true;
                break;
            }
        }
        if (!is_root) continue;
        for (const auto& decl : rule.declarations) {
            if (decl.property.rfind("--", 0) == 0) {
                vars[decl.property] = decl.value;
            }
        }
    }
    return vars;
}

// =============================================================================
// Session 8 — transformaciones 2D, tiempos y funciones de temporización
// =============================================================================

void Transform2D::to_matrix(float ox, float oy, float out[6]) const {
    const float rad = rotate_deg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(rad);
    const float sn = std::sin(rad);
    // M = T(origen) * T(translate) * R(rotate) * S(scale) * T(-origen)
    const float a = cs * scale_x;
    const float b = sn * scale_x;
    const float c = -sn * scale_y;
    const float d = cs * scale_y;
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
    out[4] = translate_x + ox - (a * ox + c * oy);
    out[5] = translate_y + oy - (b * ox + d * oy);
}

Transform2D StyleEngine::parse_transform(std::string_view val) {
    Transform2D t;
    std::string low = to_lower(val);
    if (low.empty() || low == "none") return t;

    for (const auto& [fn, args] : split_functions(val)) {
        auto nums = split_top_level_commas(args);
        auto num_at = [&](size_t i, float fb) -> float {
            if (i >= nums.size()) return fb;
            return static_cast<float>(std::atof(nums[i].c_str()));
        };
        auto px_at = [&](size_t i, float fb) -> float {
            if (i >= nums.size()) return fb;
            return StyleEngine::parse_px(nums[i], fb);
        };
        if (fn == "translate") {
            t.translate_x += px_at(0, 0.0f);
            t.translate_y += px_at(1, 0.0f);
            t.enabled = true;
        } else if (fn == "translatex") {
            t.translate_x += px_at(0, 0.0f);
            t.enabled = true;
        } else if (fn == "translatey") {
            t.translate_y += px_at(0, 0.0f);
            t.enabled = true;
        } else if (fn == "scale") {
            float sx = num_at(0, 1.0f);
            t.scale_x *= sx;
            t.scale_y *= num_at(1, sx);
            t.enabled = true;
        } else if (fn == "scalex") {
            t.scale_x *= num_at(0, 1.0f);
            t.enabled = true;
        } else if (fn == "scaley") {
            t.scale_y *= num_at(0, 1.0f);
            t.enabled = true;
        } else if (fn == "rotate" || fn == "rotatez") {
            // Admite deg (por defecto), rad y turn.
            std::string a = nums.empty() ? std::string() : to_lower(nums[0]);
            float v = a.empty() ? 0.0f : static_cast<float>(std::atof(a.c_str()));
            if (a.find("rad") != std::string::npos) v = v * 180.0f / 3.14159265358979323846f;
            else if (a.find("turn") != std::string::npos) v = v * 360.0f;
            t.rotate_deg += v;
            t.enabled = true;
        }
    }
    if (t.is_identity()) t.enabled = false;
    return t;
}

float StyleEngine::parse_time_ms(std::string_view val, float fallback) {
    std::string v = to_lower(trim(val));
    if (v.empty()) return fallback;
    char* end = nullptr;
    double num = std::strtod(v.c_str(), &end);
    if (end == v.c_str()) return fallback;
    std::string unit = trim(std::string(end));
    if (unit == "ms") return static_cast<float>(num);
    if (unit == "s" || unit.empty()) return static_cast<float>(num * 1000.0);
    return static_cast<float>(num);
}

Easing Easing::parse(std::string_view value, Easing fallback) {
    std::string v = to_lower(trim(value));
    if (v.empty()) return fallback;
    if (v == "linear") return Easing::linear();
    if (v == "ease") return Easing::ease();
    if (v == "ease-in") return Easing::ease_in();
    if (v == "ease-out") return Easing::ease_out();
    if (v == "ease-in-out") return Easing::ease_in_out();
    if (v == "step-start") { Easing e; e.kind = EasingKind::StepStart; e.steps = 1; return e; }
    if (v == "step-end") { Easing e; e.kind = EasingKind::StepEnd; e.steps = 1; return e; }
    if (v.rfind("cubic-bezier", 0) == 0) {
        size_t lp = v.find('(');
        size_t rp = v.rfind(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
            auto parts = split_top_level_commas(std::string_view(v).substr(lp + 1, rp - lp - 1));
            if (parts.size() == 4) {
                Easing e;
                e.kind = EasingKind::CubicBezier;
                e.x1 = std::clamp(static_cast<float>(std::atof(parts[0].c_str())), 0.0f, 1.0f);
                e.y1 = static_cast<float>(std::atof(parts[1].c_str()));
                e.x2 = std::clamp(static_cast<float>(std::atof(parts[2].c_str())), 0.0f, 1.0f);
                e.y2 = static_cast<float>(std::atof(parts[3].c_str()));
                return e;
            }
        }
        return fallback;
    }
    if (v.rfind("steps", 0) == 0) {
        size_t lp = v.find('(');
        size_t rp = v.rfind(')');
        Easing e;
        e.kind = EasingKind::StepEnd;
        e.steps = 1;
        if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
            auto parts = split_top_level_commas(std::string_view(v).substr(lp + 1, rp - lp - 1));
            if (!parts.empty()) e.steps = std::max(1, std::atoi(parts[0].c_str()));
            if (parts.size() > 1 && (parts[1].find("start") != std::string::npos)) {
                e.kind = EasingKind::StepStart;
            }
        }
        return e;
    }
    return fallback;
}

const CssKeyframesRule* CssStylesheet::find_keyframes(std::string_view name) const {
    for (const auto& kf : keyframes_) {
        if (kf.name == name) return &kf;
    }
    return nullptr;
}

// Parsea el cuerpo de un @keyframes: cada selector de paso ("from", "to",
// "0%", "50%, 75%") se normaliza a uno o varios offsets en 0..1.
void CssStylesheet::parse_keyframes_block(std::string_view header, std::string_view body) {
    // header ya viene en minúsculas: "@keyframes nombre" (o @-prefijado).
    std::string h(header);
    size_t sp = h.find_first_of(" \t");
    if (sp == std::string::npos) return;
    std::string name = trim(h.substr(sp + 1));
    if (name.empty()) return;

    CssKeyframesRule rule;
    rule.name = name;

    size_t pos = 0;
    while (pos < body.size()) {
        size_t lbrace = body.find('{', pos);
        if (lbrace == std::string_view::npos) break;
        std::string_view sel = body.substr(pos, lbrace - pos);
        size_t rbrace = body.find('}', lbrace + 1);
        if (rbrace == std::string_view::npos) break;
        std::string_view decl_body = body.substr(lbrace + 1, rbrace - lbrace - 1);
        pos = rbrace + 1;

        auto decls = parse_declarations(decl_body);
        if (decls.empty()) continue;

        for (const auto& piece : split_top_level_commas(sel)) {
            std::string sl = to_lower(trim(piece));
            float offset = -1.0f;
            if (sl == "from") offset = 0.0f;
            else if (sl == "to") offset = 1.0f;
            else if (!sl.empty()) {
                offset = static_cast<float>(std::atof(sl.c_str())) / 100.0f;
            }
            if (offset < 0.0f || offset > 1.0f) continue;
            rule.keyframes.push_back(CssKeyframe{offset, decls});
        }
    }

    if (rule.keyframes.empty()) return;
    std::stable_sort(rule.keyframes.begin(), rule.keyframes.end(),
                     [](const CssKeyframe& a, const CssKeyframe& b) { return a.offset < b.offset; });

    // Una redefinición del mismo nombre sustituye a la anterior (cascada CSS).
    for (auto& existing : keyframes_) {
        if (existing.name == rule.name) {
            existing = std::move(rule);
            return;
        }
    }
    keyframes_.push_back(std::move(rule));
}

ComputedStyle StyleEngine::compute_style(const DomNode* node,
                                         const CssStylesheet& sheet,
                                         const ComputedStyle* parent_style,
                                         float viewport_w,
                                         const StyleAdjuster* adjuster) {
    ComputedStyle st;
    // Inherit text properties & CSS custom properties from parent
    if (parent_style) {
        st.color = parent_style->color;
        st.font_size = parent_style->font_size;
        st.bold = parent_style->bold;
        st.monospace = parent_style->monospace;
        st.line_height = parent_style->line_height;
        st.text_align = parent_style->text_align;
        st.css_vars = parent_style->css_vars;
    }

    apply_ua_defaults(node, st);

    if (!node || node->type != NodeType::Element) return st;

    struct MatchedRule {
        int specificity;
        size_t order;
        const CssRule* rule;
    };
    std::vector<MatchedRule> matched;
    for (const auto& rule : sheet.rules()) {
        if (rule.media_min_width >= 0.0f && viewport_w < rule.media_min_width) continue;
        if (rule.media_max_width >= 0.0f && viewport_w > rule.media_max_width) continue;
        int best_spec = -1;
        for (const auto& sel : rule.selectors) {
            int spec = 0;
            if (match_selector(node, sel, spec)) {
                best_spec = std::max(best_spec, spec);
            }
        }
        if (best_spec >= 0) {
            matched.push_back(MatchedRule{best_spec, rule.source_order, &rule});
        }
    }

    std::sort(matched.begin(), matched.end(), [](const MatchedRule& a, const MatchedRule& b) {
        if (a.specificity != b.specificity) return a.specificity < b.specificity;
        return a.order < b.order;
    });

    std::vector<CssDeclaration> inline_decls;
    std::string inline_css = node->get_attr("style");
    if (!inline_css.empty()) {
        inline_decls = parse_declarations(inline_css);
    }

    // Pass 1: collect all CSS custom property (--*) definitions in cascade order
    for (const auto& m : matched) {
        for (const auto& decl : m.rule->declarations) {
            if (decl.property.rfind("--", 0) == 0) {
                st.css_vars[decl.property] = resolve_var_expr(decl.value, st.css_vars);
            }
        }
    }
    for (const auto& decl : inline_decls) {
        if (decl.property.rfind("--", 0) == 0) {
            st.css_vars[decl.property] = resolve_var_expr(decl.value, st.css_vars);
        }
    }

    // Pass 2: resolve var(...) and apply standard declarations in cascade order
    auto apply_resolved = [&](const CssDeclaration& decl) {
        if (decl.property.rfind("--", 0) == 0) return;
        if (decl.value.find("var(") != std::string::npos) {
            CssDeclaration resolved{decl.property, resolve_var_expr(decl.value, st.css_vars)};
            if (!resolved.value.empty()) apply_declaration(st, resolved);
        } else {
            apply_declaration(st, decl);
        }
    };

    for (const auto& m : matched) {
        for (const auto& decl : m.rule->declarations) {
            apply_resolved(decl);
        }
    }
    for (const auto& decl : inline_decls) {
        apply_resolved(decl);
    }

    // Pass 3 (Session 8): el efecto animado se aplica por encima de la cascada
    // del autor y del atributo `style`, como exige CSS Animations Level 1.
    if (adjuster) {
        std::vector<CssDeclaration> animated;
        adjuster->collect(node, animated);
        for (const auto& decl : animated) {
            apply_resolved(decl);
        }
    }

    return st;
}

} // namespace astra
