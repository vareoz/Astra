#include "style/css_engine.hpp"

#include <algorithm>
#include <cctype>
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
            std::string prop = to_lower(trim(item.substr(0, colon)));
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

bool match_compound_selector(const DomNode* node, std::string_view part, int& specificity) {
    if (!node || node->type != NodeType::Element || part.empty()) return false;
    if (part == "*") return true;

    std::string tag;
    std::string id;
    std::vector<std::string> classes;

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
            // Skip pseudo-classes for static matching
            while (i < part.size() && part[i] != '.' && part[i] != '#') ++i;
        } else {
            size_t st = i;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':') ++i;
            tag = to_lower(part.substr(st, i - st));
        }
    }

    if (!tag.empty() && node->tag != tag) return false;
    if (!id.empty() && node->id() != id) return false;
    for (const auto& c : classes) {
        if (!node->has_class(c)) return false;
    }

    if (!id.empty()) specificity += 100;
    specificity += static_cast<int>(classes.size()) * 10;
    if (!tag.empty()) specificity += 1;
    return true;
}

bool match_selector(const DomNode* node, std::string_view selector, int& specificity) {
    auto parts = split_ws(selector);
    if (parts.empty()) return false;
    specificity = 0;
    if (!match_compound_selector(node, parts.back(), specificity)) return false;

    const DomNode* ancestor = node->parent;
    for (int idx = static_cast<int>(parts.size()) - 2; idx >= 0; --idx) {
        bool found = false;
        while (ancestor && ancestor->type == NodeType::Element) {
            int sub_spec = 0;
            if (match_compound_selector(ancestor, parts[static_cast<size_t>(idx)], sub_spec)) {
                specificity += sub_spec;
                found = true;
                ancestor = ancestor->parent;
                break;
            }
            ancestor = ancestor->parent;
        }
        if (!found) return false;
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

void apply_declaration(ComputedStyle& st, const CssDeclaration& d) {
    const std::string& p = d.property;
    const std::string& v = d.value;
    std::string vl = to_lower(v);

    if (p == "display") {
        if (vl == "none") st.display = DisplayType::None;
        else if (vl == "flex") st.display = DisplayType::Flex;
        else if (vl == "inline" || vl == "inline-block") st.display = DisplayType::Inline;
        else st.display = DisplayType::Block;
    } else if (p == "flex-direction") {
        st.flex_direction = (vl == "column") ? FlexDirection::Column : FlexDirection::Row;
    } else if (p == "gap") {
        st.gap = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "width") {
        if (vl != "auto" && vl.find('%') == std::string::npos) st.width = StyleEngine::parse_px(v, -1.0f);
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
    } else if (p == "background" || p == "background-color") {
        auto toks = split_ws(v);
        if (!toks.empty()) st.background = StyleEngine::parse_color(toks[0], st.background);
    } else if (p == "color") {
        st.color = StyleEngine::parse_color(v, st.color);
    } else if (p == "border") {
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
    } else if (p == "font-size") {
        st.font_size = std::clamp(StyleEngine::parse_px(v, st.font_size), 8.0f, 64.0f);
    } else if (p == "font-weight") {
        st.bold = (vl == "bold" || vl == "600" || vl == "700" || vl == "800" || vl == "900");
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

        // Skip @rules like @media/@keyframes cleanly
        if (css[pos] == '@') {
            size_t brace = css.find('{', pos);
            size_t semi = css.find(';', pos);
            if (semi != std::string_view::npos && (brace == std::string_view::npos || semi < brace)) {
                pos = semi + 1;
                continue;
            }
            if (brace != std::string_view::npos) {
                int depth = 1;
                pos = brace + 1;
                while (pos < n && depth > 0) {
                    if (css[pos] == '{') ++depth;
                    else if (css[pos] == '}') --depth;
                    ++pos;
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

ComputedStyle StyleEngine::compute_style(const DomNode* node,
                                         const CssStylesheet& sheet,
                                         const ComputedStyle* parent_style) {
    ComputedStyle st;
    // Inherit text properties from parent
    if (parent_style) {
        st.color = parent_style->color;
        st.font_size = parent_style->font_size;
        st.bold = parent_style->bold;
        st.line_height = parent_style->line_height;
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

    for (const auto& m : matched) {
        for (const auto& decl : m.rule->declarations) {
            apply_declaration(st, decl);
        }
    }

    // Inline style="..." overrides stylesheet rules (specificity = 1000)
    std::string inline_css = node->get_attr("style");
    if (!inline_css.empty()) {
        for (const auto& decl : parse_declarations(inline_css)) {
            apply_declaration(st, decl);
        }
    }

    return st;
}

} // namespace astra
