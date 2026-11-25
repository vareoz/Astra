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

bool match_selector(const DomNode* node, std::string_view selector, int& specificity, int depth = 0);

// Divide por comas en el nivel superior respetando paréntesis, corchetes y
// comillas, de modo que "a, b(1, 2), c[x=',']" produce tres piezas.
std::vector<std::string> split_top_level_commas(std::string_view s) {
    std::vector<std::string> out;
    int pdepth = 0;
    int bdepth = 0;
    char quote = 0;
    size_t seg = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size()) {
            char c = s[i];
            if (quote) {
                if (c == quote && (i == 0 || s[i - 1] != '\\')) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') ++pdepth;
            else if (c == ')' && pdepth > 0) --pdepth;
            else if (c == '[') ++bdepth;
            else if (c == ']' && bdepth > 0) --bdepth;
        }
        if (i == s.size() || (s[i] == ',' && pdepth == 0 && bdepth == 0 && !quote)) {
            std::string tok = trim(s.substr(seg, i - seg));
            if (!tok.empty()) out.push_back(std::move(tok));
            seg = i + 1;
        }
    }
    return out;
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

bool match_compound_selector(const DomNode* node, std::string_view part, int& specificity, int depth = 0) {
    if (!node || node->type != NodeType::Element || part.empty() || depth > 6) return false;
    if (part == "*") return true;
    if (to_lower(part) == ":root") {
        if (node->tag == "html" || node->tag == "body") {
            specificity += 10;
            return true;
        }
        return false;
    }

    struct AttrCond {
        std::string name;
        std::string op; // "", "=", "~=", "|=", "^=", "$=", "*="
        std::string value;
        bool icase = false;
    };

    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::vector<std::string> pseudos;
    std::vector<AttrCond> attrs;

    size_t i = 0;
    while (i < part.size()) {
        if (part[i] == '#') {
            ++i;
            std::string tok;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':' && part[i] != '[') {
                if (part[i] == '\\' && i + 1 < part.size()) ++i;
                tok.push_back(part[i++]);
            }
            id = std::move(tok);
        } else if (part[i] == '.') {
            ++i;
            std::string tok;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':' && part[i] != '[') {
                if (part[i] == '\\' && i + 1 < part.size()) ++i;
                tok.push_back(part[i++]);
            }
            if (!tok.empty()) classes.push_back(std::move(tok));
        } else if (part[i] == '[') {
            ++i;
            size_t st = i;
            char q = 0;
            while (i < part.size()) {
                if (q) {
                    if (part[i] == q && (i == st || part[i - 1] != '\\')) q = 0;
                } else if (part[i] == '"' || part[i] == '\'') {
                    q = part[i];
                } else if (part[i] == ']') {
                    break;
                }
                ++i;
            }
            std::string inside = trim(part.substr(st, i - st));
            if (i < part.size() && part[i] == ']') ++i;
            if (!inside.empty()) {
                AttrCond ac;
                size_t eq = inside.find('=');
                if (eq == std::string::npos) {
                    ac.name = to_lower(trim(inside));
                } else {
                    size_t op_start = eq;
                    if (eq > 0 && (inside[eq - 1] == '~' || inside[eq - 1] == '|' ||
                                   inside[eq - 1] == '^' || inside[eq - 1] == '$' || inside[eq - 1] == '*')) {
                        op_start = eq - 1;
                    }
                    ac.name = to_lower(trim(inside.substr(0, op_start)));
                    ac.op = inside.substr(op_start, eq - op_start + 1);
                    std::string val = trim(inside.substr(eq + 1));
                    if (val.size() >= 2 && (val.back() == 'i' || val.back() == 'I') &&
                        std::isspace(static_cast<unsigned char>(val[val.size() - 2]))) {
                        ac.icase = true;
                        val = trim(val.substr(0, val.size() - 1));
                    }
                    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                            (val.front() == '\'' && val.back() == '\''))) {
                        val = val.substr(1, val.size() - 2);
                    }
                    ac.value = std::move(val);
                }
                if (!ac.name.empty()) attrs.push_back(std::move(ac));
            }
        } else if (part[i] == ':') {
            size_t st = i;
            ++i;
            if (i < part.size() && part[i] == ':') ++i; // ::before / ::after
            int pdepth = 0;
            while (i < part.size()) {
                char c = part[i];
                if (c == '(') ++pdepth;
                else if (c == ')') { if (pdepth > 0) --pdepth; }
                else if (pdepth == 0 && (c == '.' || c == '#' || c == ':' || c == '[')) break;
                ++i;
            }
            pseudos.emplace_back(part.substr(st, i - st));
        } else if (part[i] == '*') {
            ++i;
        } else {
            size_t st = i;
            while (i < part.size() && part[i] != '.' && part[i] != '#' && part[i] != ':' && part[i] != '[') ++i;
            tag = to_lower(part.substr(st, i - st));
        }
    }

    if (!tag.empty() && tag != "*" && node->tag != tag) return false;
    if (!id.empty() && node->id() != id) return false;
    for (const auto& c : classes) {
        if (!node->has_class(c)) return false;
    }

    for (const auto& ac : attrs) {
        if (!node->has_attr(ac.name)) return false;
        if (ac.op.empty()) continue;
        std::string actual = node->get_attr(ac.name);
        std::string target = ac.value;
        if (ac.icase) {
            actual = to_lower(actual);
            target = to_lower(target);
        }
        if (ac.op == "=") {
            if (actual != target) return false;
        } else if (ac.op == "~=") {
            auto toks = split_ws(actual);
            if (std::find(toks.begin(), toks.end(), target) == toks.end()) return false;
        } else if (ac.op == "|=") {
            if (actual != target && actual.rfind(target + "-", 0) != 0) return false;
        } else if (ac.op == "^=") {
            if (target.empty() || actual.rfind(target, 0) != 0) return false;
        } else if (ac.op == "$=") {
            if (target.empty() || actual.size() < target.size() ||
                actual.compare(actual.size() - target.size(), target.size(), target) != 0) return false;
        } else if (ac.op == "*=") {
            if (target.empty() || actual.find(target) == std::string::npos) return false;
        }
    }

    auto match_nth_expr = [](int idx_1based, const std::string& raw_expr) -> bool {
        std::string e;
        for (char c : raw_expr) {
            if (!std::isspace(static_cast<unsigned char>(c))) e.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (e == "odd") return (idx_1based % 2) == 1;
        if (e == "even") return (idx_1based % 2) == 0;
        size_t npos = e.find('n');
        if (npos == std::string::npos) {
            return idx_1based == std::atoi(e.c_str());
        }
        int a = 1;
        if (npos == 0) a = 1;
        else if (npos == 1 && e[0] == '+') a = 1;
        else if (npos == 1 && e[0] == '-') a = -1;
        else a = std::atoi(e.substr(0, npos).c_str());
        int b = (npos + 1 < e.size()) ? std::atoi(e.substr(npos + 1).c_str()) : 0;
        if (a == 0) return idx_1based == b;
        int diff = idx_1based - b;
        if (a > 0) return diff >= 0 && (diff % a) == 0;
        return diff <= 0 && (diff % (-a)) == 0;
    };

    int pseudo_spec = 0;
    for (const auto& raw_ps : pseudos) {
        std::string ps = to_lower(raw_ps);
        if (ps == "::before" || ps == "::after" || ps == ":before" || ps == ":after" ||
            ps.rfind("::", 0) == 0) {
            return false; // Pseudo-elements never match normal element pass
        } else if (ps == ":root") {
            if (node->tag != "html" && node->tag != "body") return false;
            pseudo_spec += 10;
        } else if (ps == ":first-child") {
            if (!node->parent) return false;
            const DomNode* first_el = nullptr;
            for (const DomNode* sib : node->parent->children) {
                if (sib->type == NodeType::Element) { first_el = sib; break; }
            }
            if (first_el != node) return false;
            pseudo_spec += 10;
        } else if (ps == ":last-child") {
            if (!node->parent) return false;
            const DomNode* last_el = nullptr;
            for (const DomNode* sib : node->parent->children) {
                if (sib->type == NodeType::Element) last_el = sib;
            }
            if (last_el != node) return false;
            pseudo_spec += 10;
        } else if (ps == ":first-of-type") {
            if (!node->parent) return false;
            const DomNode* first_el = nullptr;
            for (const DomNode* sib : node->parent->children) {
                if (sib->type == NodeType::Element && sib->tag == node->tag) { first_el = sib; break; }
            }
            if (first_el != node) return false;
            pseudo_spec += 10;
        } else if (ps == ":last-of-type") {
            if (!node->parent) return false;
            const DomNode* last_el = nullptr;
            for (const DomNode* sib : node->parent->children) {
                if (sib->type == NodeType::Element && sib->tag == node->tag) last_el = sib;
            }
            if (last_el != node) return false;
            pseudo_spec += 10;
        } else if (ps.rfind(":nth-child(", 0) == 0 && ps.back() == ')') {
            if (!node->parent) return false;
            int pos = 0;
            bool found = false;
            for (const DomNode* sib : node->parent->children) {
                if (sib->type == NodeType::Element) {
                    ++pos;
                    if (sib == node) { found = true; break; }
                }
            }
            if (!found) return false;
            std::string expr = ps.substr(11, ps.size() - 12);
            if (!match_nth_expr(pos, expr)) return false;
            pseudo_spec += 10;
        } else if (ps == ":empty") {
            if (!node->children.empty()) return false;
            pseudo_spec += 10;
        } else if (ps == ":focus" || ps == ":focus-visible") {
            if (node->get_attr("data-astra-focused") != "1") return false;
            pseudo_spec += 10;
        } else if (ps == ":hover") {
            if (node->get_attr("data-astra-hovered") != "1") return false;
            pseudo_spec += 10;
        } else if (ps == ":checked") {
            if (!node->has_attr("checked")) return false;
            pseudo_spec += 10;
        } else if (ps == ":disabled") {
            if (!node->has_attr("disabled")) return false;
            pseudo_spec += 10;
        } else if (ps == ":enabled") {
            if (node->has_attr("disabled")) return false;
            pseudo_spec += 10;
        } else if (ps == ":link" || ps == ":any-link") {
            if (node->tag != "a" || !node->has_attr("href")) return false;
            pseudo_spec += 10;
        } else if (ps == ":visited" || ps == ":active" || ps == ":target" || ps == ":fullscreen" || ps == ":modal") {
            return false;
        } else if (ps.rfind(":not(", 0) == 0 && raw_ps.back() == ')') {
            std::string inner = raw_ps.substr(5, raw_ps.size() - 6);
            auto list = split_top_level_commas(inner);
            int max_s = 0;
            for (const auto& sub : list) {
                int s = 0;
                if (match_selector(node, sub, s, depth + 1)) return false;
                max_s = std::max(max_s, s);
            }
            pseudo_spec += max_s;
        } else if ((ps.rfind(":is(", 0) == 0 || ps.rfind(":where(", 0) == 0) && raw_ps.back() == ')') {
            bool is_where = (ps.rfind(":where(", 0) == 0);
            size_t off = is_where ? 7 : 4;
            std::string inner = raw_ps.substr(off, raw_ps.size() - off - 1);
            auto list = split_top_level_commas(inner);
            bool any_ok = false;
            int max_s = 0;
            for (const auto& sub : list) {
                int s = 0;
                if (match_selector(node, sub, s, depth + 1)) {
                    any_ok = true;
                    max_s = std::max(max_s, s);
                }
            }
            if (!any_ok) return false;
            if (!is_where) pseudo_spec += max_s;
        } else {
            // CRITICAL: unknown pseudo-classes MUST NOT silently match all elements!
            return false;
        }
    }

    if (!id.empty()) specificity += 100;
    specificity += static_cast<int>(classes.size() + attrs.size()) * 10 + pseudo_spec;
    if (!tag.empty() && tag != "*") specificity += 1;
    return true;
}

bool match_selector(const DomNode* node, std::string_view selector, int& specificity, int depth) {
    if (depth > 6) return false;
    // Tokenize selector supporting descendant (' '), direct-child ('>'), adjacent ('+'), and general sibling ('~')
    std::vector<std::string> parts;
    std::vector<char> comb; // comb[i] applies between parts[i] and parts[i+1]
    size_t i = 0;
    char pending_comb = ' ';
    while (i < selector.size()) {
        while (i < selector.size() && std::isspace(static_cast<unsigned char>(selector[i]))) ++i;
        if (i >= selector.size()) break;
        if (selector[i] == '>' || selector[i] == '+' || selector[i] == '~') {
            pending_comb = selector[i];
            ++i;
            continue;
        }
        size_t st = i;
        int pdepth = 0;
        int bdepth = 0;
        char quote = 0;
        while (i < selector.size()) {
            char c = selector[i];
            if (quote) {
                if (c == quote && (i == st || selector[i - 1] != '\\')) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') ++pdepth;
            else if (c == ')' && pdepth > 0) --pdepth;
            else if (c == '[') ++bdepth;
            else if (c == ']' && bdepth > 0) --bdepth;
            else if (pdepth == 0 && bdepth == 0 &&
                     (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '+' || c == '~')) {
                break;
            }
            ++i;
        }
        if (i > st) {
            if (!parts.empty()) comb.push_back(pending_comb);
            parts.emplace_back(selector.substr(st, i - st));
            pending_comb = ' ';
        }
    }
    if (parts.empty()) return false;
    specificity = 0;
    if (!match_compound_selector(node, parts.back(), specificity, depth)) return false;

    const DomNode* cur = node;
    for (int idx = static_cast<int>(parts.size()) - 2; idx >= 0; --idx) {
        char c = comb[static_cast<size_t>(idx)];
        if (c == '>') {
            const DomNode* p = cur ? cur->parent : nullptr;
            if (!p || p->type != NodeType::Element) return false;
            int sub_spec = 0;
            if (!match_compound_selector(p, parts[static_cast<size_t>(idx)], sub_spec, depth)) return false;
            specificity += sub_spec;
            cur = p;
        } else if (c == '+' || c == '~') {
            const DomNode* p = cur ? cur->parent : nullptr;
            if (!p) return false;
            int my_idx = -1;
            for (size_t k = 0; k < p->children.size(); ++k) {
                if (p->children[k] == cur) { my_idx = static_cast<int>(k); break; }
            }
            if (my_idx <= 0) return false;
            bool found = false;
            for (int k = my_idx - 1; k >= 0; --k) {
                const DomNode* sib = p->children[static_cast<size_t>(k)];
                if (sib->type != NodeType::Element) continue;
                int sub_spec = 0;
                if (match_compound_selector(sib, parts[static_cast<size_t>(idx)], sub_spec, depth)) {
                    specificity += sub_spec;
                    cur = sib;
                    found = true;
                    break;
                }
                if (c == '+') break; // Only check immediate preceding element
            }
            if (!found) return false;
        } else {
            const DomNode* ancestor = cur ? cur->parent : nullptr;
            bool found = false;
            while (ancestor && ancestor->type == NodeType::Element) {
                int sub_spec = 0;
                if (match_compound_selector(ancestor, parts[static_cast<size_t>(idx)], sub_spec, depth)) {
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
        } else if (s.compare(pos, 7, "minmax(") == 0) {
            // minmax(min, max): el tamaño que manda para repartir el espacio es
            // el máximo, así que `minmax(0, 1fr)` se comporta como `1fr`.
            int depth = 1;
            size_t q = pos + 7;
            while (q < s.size() && depth > 0) {
                if (s[q] == '(') ++depth;
                else if (s[q] == ')') --depth;
                if (depth > 0) ++q;
            }
            std::string inner = s.substr(pos + 7, q - (pos + 7));
            size_t comma = inner.rfind(',');
            std::string upper = trim(comma == std::string::npos ? inner : inner.substr(comma + 1));
            GridTrack tr;
            if (upper.find("fr") != std::string::npos) {
                tr.is_fr = true;
                tr.value = std::max(0.1f, std::strtof(upper.c_str(), nullptr));
            } else if (upper == "auto" || upper == "max-content" || upper == "min-content") {
                tr.is_fr = true;
                tr.value = 1.0f;
            } else {
                tr.is_fr = false;
                tr.value = std::max(24.0f, StyleEngine::parse_px(upper, 120.0f));
            }
            tracks.push_back(tr);
            pos = (q < s.size()) ? q + 1 : s.size();
        } else {
            size_t st = pos;
            while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            std::string tok = s.substr(st, pos - st);
            GridTrack tr;
            if (tok == "min-content" || tok == "max-content" || tok == "fit-content") {
                // Sin cálculo intrínseco de pistas, una pista de contenido se
                // comporta como flexible: reparte lo que quede.
                tr.is_fr = true;
                tr.value = 1.0f;
                tracks.push_back(tr);
                continue;
            }
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
        else if (vl == "inline-block") st.display = DisplayType::InlineBlock;
        else if (vl == "inline") st.display = DisplayType::Inline;
        else if (vl == "table") st.display = DisplayType::Table;
        else if (vl == "table-row") st.display = DisplayType::TableRow;
        else if (vl == "table-cell") st.display = DisplayType::TableCell;
        else st.display = DisplayType::Block;
    } else if (p == "float") {
        if (vl == "left") st.float_mode = FloatMode::Left;
        else if (vl == "right") st.float_mode = FloatMode::Right;
        else st.float_mode = FloatMode::None;
    } else if (p == "clear") {
        if (vl == "left") st.clear_mode = ClearMode::Left;
        else if (vl == "right") st.clear_mode = ClearMode::Right;
        else if (vl == "both") st.clear_mode = ClearMode::Both;
        else st.clear_mode = ClearMode::None;
    } else if (p == "content") {
        st.has_content = (vl != "none" && vl != "normal");
        if (st.has_content) {
            std::string ct = v;
            if (ct.size() >= 2 && ((ct.front() == '"' && ct.back() == '"') || (ct.front() == '\'' && ct.back() == '\''))) {
                ct = ct.substr(1, ct.size() - 2);
            }
            st.content_text = ct;
        }
    } else if (p == "flex-direction") {
        st.flex_direction = (vl == "column") ? FlexDirection::Column : FlexDirection::Row;
    } else if (p == "flex-wrap") {
        st.flex_wrap = (vl == "wrap" || vl == "wrap-reverse");
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
    } else if (p == "grid-template-areas") {
        // Cada cadena entrecomillada es una fila de la plantilla.
        st.grid_areas.clear();
        bool in_str = false;
        char q = 0;
        std::string cur;
        for (char c : v) {
            if (!in_str && (c == '"' || c == '\'')) { in_str = true; q = c; cur.clear(); }
            else if (in_str && c == q) {
                in_str = false;
                std::vector<std::string> row = split_ws(cur);
                if (!row.empty()) st.grid_areas.push_back(std::move(row));
            } else if (in_str) cur.push_back(c);
        }
    } else if (p == "grid-template") {
        // Atajo `filas / columnas`: las columnas son lo que hay tras la barra.
        // Si además lleva cadenas, son áreas con nombre.
        size_t slash = v.rfind('/');
        if (slash != std::string::npos) {
            st.grid_columns = parse_grid_tracks(std::string_view(v).substr(slash + 1));
        }
        if (v.find('"') != std::string::npos || v.find('\'') != std::string::npos) {
            CssDeclaration areas{"grid-template-areas", v};
            apply_declaration(st, areas);
        }
    } else if (p == "grid-area") {
        // Forma corta con un único identificador (la que usan las skins).
        // CSS Values §4.2: los identificadores de autor distinguen mayúsculas,
        // así que aquí se usa el valor crudo y NO el normalizado a minúsculas:
        // la plantilla declara `pageContent`, no `pagecontent`.
        std::string name = trim(v);
        if (!name.empty() && name.find('/') == std::string::npos &&
            !std::isdigit(static_cast<unsigned char>(name[0]))) {
            st.grid_area = name;
        }
    } else if (p == "column-gap" || p == "grid-column-gap") {
        st.column_gap = std::max(0.0f, StyleEngine::parse_px(v, 0.0f));
    } else if (p == "row-gap" || p == "grid-row-gap") {
        st.row_gap = std::max(0.0f, StyleEngine::parse_px(v, 0.0f));
    } else if (p == "gap" || p == "grid-gap") {
        st.gap = StyleEngine::parse_px(v, 0.0f);
    } else if (p == "width") {
        st.width = -1.0f;
        st.width_pct = -1.0f;
        st.width_calc.clear();
        if (vl.rfind("calc(", 0) == 0) {
            st.width_calc = v;
        } else if (vl != "auto" && vl.find('%') != std::string::npos) {
            st.width_pct = std::clamp(std::strtof(vl.c_str(), nullptr), 0.0f, 100.0f);
        } else if (vl != "auto") {
            st.width = StyleEngine::parse_px(v, -1.0f);
        }
    } else if (p == "min-width") {
        if (vl != "auto" && vl.find('%') == std::string::npos) st.min_width = StyleEngine::parse_px(v, -1.0f);
    } else if (p == "max-width") {
        if (vl != "none" && vl.find('%') == std::string::npos) st.max_width = StyleEngine::parse_px(v, -1.0f);
    } else if (p == "height") {
        if (vl.rfind("calc(", 0) == 0) {
            st.height = StyleEngine::eval_calc(v, 720.0f, 1280.0f, -1.0f);
        } else if (vl != "auto" && vl.find('%') == std::string::npos) {
            st.height = StyleEngine::parse_px(v, -1.0f);
        }
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
    } else if (p == "border-left") {
        auto toks = split_ws(v);
        for (const auto& t : toks) {
            if (std::isdigit(static_cast<unsigned char>(t[0]))) {
                st.border_left_width = StyleEngine::parse_px(t, 3.0f);
            } else if (t != "solid" && t != "dashed" && t != "dotted" && t != "none") {
                st.border_left_color = StyleEngine::parse_color(t, Color{56, 189, 248, 255});
            }
        }
    } else if (p == "border" || p == "border-top" || p == "border-right" || p == "border-bottom") {
        if (vl == "none" || vl == "0" || vl == "0px") {
            if (p == "border") st.border_width = 0.0f;
        } else {
            auto toks = split_ws(v);
            for (const auto& t : toks) {
                if (t == "none") {
                    if (p == "border") st.border_width = 0.0f;
                } else if (std::isdigit(static_cast<unsigned char>(t[0]))) {
                    st.border_width = StyleEngine::parse_px(t, 1.0f);
                } else if (t != "solid" && t != "dashed" && t != "dotted") {
                    st.border_color = StyleEngine::parse_color(t, Color{80, 90, 110, 255});
                }
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

void apply_ua_defaults(const DomNode* node, ComputedStyle& st, bool light_web_default) {
    if (!node || node->type != NodeType::Element) return;
    const std::string& t = node->tag;
    if (t == "head" || t == "style" || t == "script" || t == "meta" || t == "link" ||
        t == "title" || t == "noscript" || t == "template") {
        st.display = DisplayType::None;
    } else if (t == "span" || t == "a" || t == "strong" || t == "b" || t == "em" ||
               t == "i" || t == "small" || t == "time" || t == "label" || t == "abbr" ||
               t == "cite" || t == "sup" || t == "sub" || t == "mark" || t == "font" || t == "code") {
        st.display = DisplayType::Inline;
        st.border_width = 0.0f;
        if (t == "a") {
            st.color = light_web_default ? Color{29, 78, 216, 255} : Color{96, 175, 255, 255};
        }
        if (t == "strong" || t == "b") st.bold = true;
        if (t == "small" || t == "sup" || t == "sub") st.font_size = std::max(10.0f, st.font_size * 0.85f);
        if (t == "code") {
            st.background = light_web_default ? Color{241, 245, 249, 255} : Color{28, 34, 46, 255};
            st.color = light_web_default ? Color{15, 23, 42, 255} : Color{135, 232, 182, 255};
            st.padding = {2, 5, 2, 5};
            st.monospace = true;
        }
    } else if (t == "img" || t == "svg") {
        st.display = DisplayType::InlineBlock;
    } else if (t == "body") {
        if (light_web_default) {
            st.background = Color{255, 255, 255, 255};
            st.color = Color{17, 24, 39, 255};
            st.padding = {12, 20, 20, 20};
            st.font_size = 14.0f;
        } else {
            st.background = Color{13, 17, 24, 255};
            st.color = Color{226, 232, 240, 255};
            st.padding = {24, 28, 28, 28};
            st.font_size = 15.0f;
        }
    } else if (t == "h1") {
        st.font_size = 28.0f;
        st.bold = true;
        st.margin = {8, 0, 14, 0};
        st.color = light_web_default ? Color{15, 23, 42, 255} : Color{248, 250, 252, 255};
    } else if (t == "h2") {
        st.font_size = 22.0f;
        st.bold = true;
        st.margin = {14, 0, 10, 0};
        st.color = light_web_default ? Color{15, 23, 42, 255} : Color{241, 245, 249, 255};
    } else if (t == "h3") {
        st.font_size = 17.0f;
        st.bold = true;
        st.margin = {10, 0, 8, 0};
    } else if (t == "p") {
        st.margin = {0, 0, 10, 0};
    } else if (t == "pre") {
        st.background = light_web_default ? Color{248, 250, 252, 255} : Color{18, 23, 33, 255};
        st.color = light_web_default ? Color{15, 23, 42, 255} : Color{166, 226, 194, 255};
        st.padding = {12, 14, 12, 14};
        st.border_radius = 8.0f;
        st.border_width = 1.0f;
        st.border_color = light_web_default ? Color{203, 213, 225, 255} : Color{45, 56, 76, 255};
        st.margin = {6, 0, 12, 0};
        st.monospace = true;
    } else if (t == "input" || t == "textarea") {
        std::string itype = (t == "input") ? node->get_attr("type") : std::string();
        std::string itype_low;
        itype_low.reserve(itype.size());
        for (char c : itype) itype_low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (itype_low == "hidden") {
            st.display = DisplayType::None;
            return;
        }
        st.background = light_web_default ? Color{255, 255, 255, 255} : Color{15, 23, 42, 255};
        st.color = light_web_default ? Color{15, 23, 42, 255} : Color{241, 245, 249, 255};
        st.border_width = 1.0f;
        st.border_color = light_web_default ? Color{203, 213, 225, 255} : Color{51, 65, 85, 255};
        st.border_radius = (itype_low == "radio") ? 7.0f : ((itype_low == "checkbox") ? 3.0f : 6.0f);
        st.padding = (itype_low == "checkbox" || itype_low == "radio") ? EdgeSizes{0, 0, 0, 0} : EdgeSizes{6, 10, 6, 10};
        st.font_size = 14.0f;
        if (itype_low == "checkbox" || itype_low == "radio") {
            st.width = 14.0f;
            st.height = 14.0f;
            st.display = DisplayType::InlineBlock;
        } else {
            st.height = (t == "textarea") ? 88.0f : (light_web_default ? 32.0f : 38.0f);
        }
    } else if (t == "ul" || t == "ol") {
        st.padding = {0, 0, 0, 20};
        st.margin = {4, 0, 10, 0};
    } else if (t == "li") {
        st.margin = {0, 0, 5, 0};
    } else if (t == "blockquote") {
        st.padding = {10, 14, 10, 16};
        st.margin = {8, 0, 12, 0};
        st.background = light_web_default ? Color{248, 250, 252, 255} : Color{22, 29, 42, 255};
        st.border_width = 2.0f;
        st.border_color = light_web_default ? Color{148, 163, 184, 255} : Color{56, 189, 248, 255};
        st.border_radius = 6.0f;
    } else if (t == "table") {
        st.display = DisplayType::Table;
        bool borderless = (node->get_attr("border") == "0") ||
                          (light_web_default && !node->has_attr("border"));
        st.border_width = borderless ? 0.0f : 1.0f;
        st.border_color = light_web_default ? Color{203, 213, 225, 255} : Color{51, 65, 85, 255};
        st.margin = borderless ? EdgeSizes{0, 0, 4, 0} : EdgeSizes{8, 0, 12, 0};
    } else if (t == "tr") {
        st.display = DisplayType::TableRow;
    } else if (t == "th") {
        st.display = DisplayType::TableCell;
        st.bold = true;
        st.background = light_web_default ? Color{241, 245, 249, 255} : Color{23, 37, 64, 255};
        st.color = light_web_default ? Color{15, 23, 42, 255} : Color{241, 245, 249, 255};
        st.padding = {8, 12, 8, 12};
        st.border_width = 1.0f;
        st.border_color = light_web_default ? Color{203, 213, 225, 255} : Color{51, 65, 85, 255};
    } else if (t == "td") {
        st.display = DisplayType::TableCell;
        bool parent_borderless = false;
        for (const DomNode* p = node->parent; p; p = p->parent) {
            if (p->tag == "table") {
                parent_borderless = (p->get_attr("border") == "0") ||
                                    (light_web_default && !p->has_attr("border"));
                break;
            }
        }
        st.padding = parent_borderless ? EdgeSizes{3, 6, 3, 6} : EdgeSizes{7, 12, 7, 12};
        st.border_width = parent_borderless ? 0.0f : 1.0f;
        st.border_color = light_web_default ? Color{226, 232, 240, 255} : Color{30, 41, 59, 255};
    } else if (t == "iframe") {
        st.display = DisplayType::Block;
        st.border_width = 1.0f;
        st.border_color = light_web_default ? Color{203, 213, 225, 255} : Color{51, 65, 85, 255};
        st.border_radius = 6.0f;
        st.background = light_web_default ? Color{248, 250, 252, 255} : Color{11, 17, 30, 255};
        st.overflow = OverflowMode::Hidden;
    } else if (t == "hr") {
        st.height = 1.0f;
        st.background = light_web_default ? Color{203, 213, 225, 255} : Color{51, 65, 85, 255};
        st.margin = {12, 0, 12, 0};
    }

    // Apply legacy HTML presentational attributes (overridden by author CSS cascade)
    if (node->has_attr("hidden")) {
        st.display = DisplayType::None;
    }
    std::string bg = node->get_attr("bgcolor");
    if (!bg.empty()) {
        st.background = StyleEngine::parse_color(bg, st.background);
    }
    if (t == "font") {
        std::string fc = node->get_attr("color");
        if (!fc.empty()) st.color = StyleEngine::parse_color(fc, st.color);
    }
    std::string align = to_lower(node->get_attr("align"));
    if (align == "center") st.text_align = TextAlign::Center;
    else if (align == "right") st.text_align = TextAlign::Right;
    else if (align == "left") st.text_align = TextAlign::Left;
    if (t == "table" || t == "td" || t == "th" || t == "iframe") {
        std::string wa = trim(node->get_attr("width"));
        if (!wa.empty()) {
            if (wa.find('%') != std::string::npos) {
                st.width_pct = std::clamp(std::strtof(wa.c_str(), nullptr), 0.0f, 100.0f);
            } else {
                float px = StyleEngine::parse_px(wa, -1.0f);
                if (px > 0.0f) st.width = px;
            }
        }
        std::string ha = trim(node->get_attr("height"));
        if (!ha.empty() && ha.find('%') == std::string::npos) {
            float px = StyleEngine::parse_px(ha, -1.0f);
            if (px > 0.0f) st.height = px;
        }
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

        // Parse @media, @layer, and @supports blocks; skip print media & other @rules cleanly
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
                if (inner_end > inner_start) {
                    // Session 8: @keyframes se recoge antes que el resto de
                    // @-reglas; su cuerpo son pasos, no declaraciones sueltas.
                    if (header.rfind("@keyframes", 0) == 0 ||
                        header.rfind("@-webkit-keyframes", 0) == 0 ||
                        header.rfind("@-moz-keyframes", 0) == 0) {
                        // Normaliza el prefijo de proveedor al nombre canónico.
                        std::string h = header;
                        size_t kw = h.find("keyframes");
                        if (kw != std::string::npos) h = "@keyframes" + h.substr(kw + 9);
                        parse_keyframes_block(h, css.substr(inner_start, inner_end - inner_start));
                    } else if (header.rfind("@media", 0) == 0) {
                        // Skip print-only stylesheets on screen rendering
                        bool is_print_only = (header.find("print") != std::string::npos &&
                                              header.find("screen") == std::string::npos &&
                                              header.find("not print") == std::string::npos);
                        if (!is_print_only) {
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
                    } else if (header.rfind("@layer", 0) == 0) {
                        parse_block(css.substr(inner_start, inner_end - inner_start), min_w, max_w);
                    } else if (header.rfind("@supports", 0) == 0) {
                        if (header.find("@supports not") == std::string::npos) {
                            parse_block(css.substr(inner_start, inner_end - inner_start), min_w, max_w);
                        }
                    }
                }
                continue;
            }
            break;
        }

        size_t lbrace = css.find('{', pos);
        if (lbrace == std::string_view::npos) break;
        std::string_view sel_group = css.substr(pos, lbrace - pos);
        // Find matching closing brace (handles nested CSS cleanly without corrupting subsequent rules)
        int bdepth = 1;
        size_t scan = lbrace + 1;
        while (scan < n && bdepth > 0) {
            if (css[scan] == '{') ++bdepth;
            else if (css[scan] == '}') --bdepth;
            if (bdepth > 0) ++scan;
        }
        if (bdepth != 0) break;
        size_t rbrace = scan;
        std::string_view body = css.substr(lbrace + 1, rbrace - lbrace - 1);
        pos = rbrace + 1;

        CssRule rule;
        rule.source_order = rules_.size();
        rule.media_min_width = min_w;
        rule.media_max_width = max_w;
        rule.selectors = split_top_level_commas(sel_group);
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
    if (to_lower(s).rfind("calc(", 0) == 0) {
        return eval_calc(s, 1280.0f, 1280.0f, fallback);
    }
    char* end = nullptr;
    float num = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return fallback;
    if (s.find("rem") != std::string::npos || s.find("em") != std::string::npos) {
        return num * 16.0f;
    }
    return num;
}

float StyleEngine::eval_calc(std::string_view val, float ref_px, float viewport_w, float fallback) {
    std::string s = trim(val);
    std::string sl = to_lower(s);
    if (sl.rfind("calc(", 0) == 0 && s.back() == ')') {
        s = s.substr(5, s.size() - 6);
    }
    struct CalcParser {
        const std::string& src;
        size_t pos = 0;
        float ref_px;
        float vw_px;
        bool ok = true;

        void skip_ws() {
            while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
        }
        float parse_expr() {
            float v = parse_term();
            while (ok) {
                skip_ws();
                if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
                    char op = src[pos++];
                    float r = parse_term();
                    v = (op == '+') ? (v + r) : (v - r);
                } else break;
            }
            return v;
        }
        float parse_term() {
            float v = parse_factor();
            while (ok) {
                skip_ws();
                if (pos < src.size() && (src[pos] == '*' || src[pos] == '/')) {
                    char op = src[pos++];
                    float r = parse_factor();
                    if (op == '*') v *= r;
                    else v = (std::abs(r) > 1e-6f) ? (v / r) : v;
                } else break;
            }
            return v;
        }
        float parse_factor() {
            skip_ws();
            if (pos >= src.size()) { ok = false; return 0.0f; }
            if (src[pos] == '(') {
                if (++depth > 32) { ok = false; return 0.0f; }
                ++pos;
                float v = parse_expr();
                skip_ws();
                if (pos < src.size() && src[pos] == ')') ++pos;
                --depth;
                return v;
            }
            if (src[pos] == '-') { if (++depth > 32) { ok = false; return 0.0f; } ++pos; float r = -parse_factor(); --depth; return r; }
            if (src[pos] == '+') { if (++depth > 32) { ok = false; return 0.0f; } ++pos; float r = parse_factor(); --depth; return r; }
            const char* start = src.c_str() + pos;
            char* end = nullptr;
            float num = std::strtof(start, &end);
            if (end == start) { ok = false; return 0.0f; }
            pos = static_cast<size_t>(end - src.c_str());
            size_t ust = pos;
            while (pos < src.size() && (std::isalpha(static_cast<unsigned char>(src[pos])) || src[pos] == '%')) ++pos;
            std::string unit = to_lower(src.substr(ust, pos - ust));
            if (unit == "%") return num * 0.01f * ref_px;
            if (unit == "vw") return num * 0.01f * vw_px;
            if (unit == "vh") return num * 0.01f * 720.0f;
            if (unit == "rem" || unit == "em") return num * 16.0f;
            return num;
        }
        int depth = 0;
    };
    CalcParser cp{s, 0, ref_px, viewport_w, true, 0};
    float res = cp.parse_expr();
    return (cp.ok && std::isfinite(res)) ? res : fallback;
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
                                         bool light_web_default,
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

    apply_ua_defaults(node, st, light_web_default);

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

bool StyleEngine::compute_pseudo_style(const DomNode* node,
                                       const CssStylesheet& sheet,
                                       std::string_view pseudo_name,
                                       const ComputedStyle& element_style,
                                       ComputedStyle& out_style,
                                       float viewport_w) {
    if (!node || node->type != NodeType::Element) return false;
    std::string want_double = to_lower(pseudo_name); // "::before" or "::after"
    std::string want_single = (want_double.size() > 1 && want_double[0] == ':' && want_double[1] == ':')
                                  ? want_double.substr(1)
                                  : want_double;

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
            std::string sl = to_lower(trim(sel));
            std::string base_sel;
            if (sl.size() > want_double.size() &&
                sl.compare(sl.size() - want_double.size(), want_double.size(), want_double) == 0) {
                base_sel = trim(sel.substr(0, sel.size() - want_double.size()));
            } else if (sl.size() > want_single.size() &&
                       sl.compare(sl.size() - want_single.size(), want_single.size(), want_single) == 0) {
                base_sel = trim(sel.substr(0, sel.size() - want_single.size()));
            } else {
                continue;
            }
            int spec = 0;
            if (match_selector(node, base_sel, spec)) {
                best_spec = std::max(best_spec, spec + 1);
            }
        }
        if (best_spec >= 0) {
            matched.push_back(MatchedRule{best_spec, rule.source_order, &rule});
        }
    }
    if (matched.empty()) return false;

    std::sort(matched.begin(), matched.end(), [](const MatchedRule& a, const MatchedRule& b) {
        if (a.specificity != b.specificity) return a.specificity < b.specificity;
        return a.order < b.order;
    });

    out_style = ComputedStyle{};
    out_style.display = DisplayType::InlineBlock;
    out_style.color = element_style.color;
    out_style.font_size = element_style.font_size;
    out_style.bold = element_style.bold;
    out_style.monospace = element_style.monospace;
    out_style.line_height = element_style.line_height;
    out_style.css_vars = element_style.css_vars;

    for (const auto& m : matched) {
        for (const auto& decl : m.rule->declarations) {
            if (decl.property.rfind("--", 0) == 0) continue;
            if (decl.value.find("var(") != std::string::npos) {
                CssDeclaration resolved{decl.property, resolve_var_expr(decl.value, out_style.css_vars)};
                if (!resolved.value.empty()) apply_declaration(out_style, resolved);
            } else {
                apply_declaration(out_style, decl);
            }
        }
    }
    return out_style.has_content && out_style.display != DisplayType::None;
}

} // namespace astra
