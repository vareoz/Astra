#include "layout/layout_engine.hpp"
#include "gfx/font_engine.hpp"
#include "ipc/ipc_channel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <cstring>
#include <cstdlib>

namespace astra {

namespace {

float line_height_for_size(float font_size) {
    return FontEngine::instance().line_height(font_size);
}

uint32_t apply_opacity(uint32_t argb, float opacity) {
    if (opacity >= 0.999f) return argb;
    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t na = static_cast<uint32_t>(std::round(static_cast<float>(a) * std::clamp(opacity, 0.0f, 1.0f)));
    return (na << 24) | (argb & 0x00FFFFFFu);
}

// `keep_leading`/`keep_trailing` conservan UN espacio en el borde de la
// primera/última línea. Hace falta porque la caja se mide con el texto crudo
// (espacios incluidos): si el dibujado los recorta, la palabra se pega a la
// anterior y sobra hueco al final. Sólo se activan cuando el nodo tiene
// vecinos en línea, no al principio o final de un bloque, donde CSS Text
// §4.1.1 sí elimina el espacio de borde.
std::vector<std::string> wrap_text(const std::string& raw, float max_width, float font_size, bool bold, bool mono,
                                   bool keep_leading = false, bool keep_trailing = false) {
    std::vector<std::string> lines;
    auto& fe = FontEngine::instance();
    float usable_w = std::max(40.0f, max_width);

    std::string cur;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '\n') {
            if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
            ++i;
            continue;
        }
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r')) ++i;
        size_t wstart = i;
        while (i < raw.size() && !std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i == wstart) break;

        std::string word = raw.substr(wstart, i - wstart);
        if (cur.empty()) {
            cur = std::move(word);
        } else {
            std::string cand = cur + " " + word;
            if (fe.measure_text(cand, font_size, bold, mono) <= usable_w) {
                cur = std::move(cand);
            } else {
                lines.push_back(std::move(cur));
                cur = std::move(word);
            }
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    if (!lines.empty()) {
        const bool raw_lead = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()));
        const bool raw_trail = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()));
        if (keep_leading && raw_lead) lines.front().insert(lines.front().begin(), ' ');
        if (keep_trailing && raw_trail) lines.back().push_back(' ');
    }
    return lines;
}

void offset_subtree(LayoutFrame* frame, float dx, float dy) {
    if (!frame || (dx == 0.0f && dy == 0.0f)) return;
    frame->border_box.x += dx;
    frame->border_box.y += dy;
    frame->content_box.x += dx;
    frame->content_box.y += dy;
    for (LayoutFrame* ch : frame->children) {
        offset_subtree(ch, dx, dy);
    }
}

Rect intersect_rect(const Rect& a, const Rect& b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);
    return Rect{x1, y1, std::max(0.0f, x2 - x1), std::max(0.0f, y2 - y1)};
}

std::string resolve_media_src(const DomNode* node) {
    if (!node) return {};
    std::string direct = node->get_attr("src");
    if (!direct.empty()) return direct;
    std::string fallback;
    for (const DomNode* ch : node->children) {
        if (ch && ch->type == NodeType::Element && ch->tag == "source") {
            std::string s = ch->get_attr("src");
            if (s.empty()) continue;
            if (fallback.empty()) fallback = s;
            std::string t = ch->get_attr("type");
            if (t.find("mp4") != std::string::npos || t.find("webm") != std::string::npos ||
                t.find("mpegurl") != std::string::npos || t.find("wav") != std::string::npos ||
                t.find("mpeg") != std::string::npos) {
                return s;
            }
        }
    }
    return fallback;
}

std::string resolve_img_src(const DomNode* node) {
    if (!node) return {};
    std::string s = node->get_attr("src");
    if (!s.empty() && s != "about:blank") return s;
    s = node->get_attr("data-src");
    if (!s.empty()) return s;
    std::string ss = node->get_attr("srcset");
    if (!ss.empty()) {
        size_t i = 0;
        while (i < ss.size() && std::isspace(static_cast<unsigned char>(ss[i]))) ++i;
        size_t st = i;
        while (i < ss.size() && !std::isspace(static_cast<unsigned char>(ss[i])) && ss[i] != ',') ++i;
        if (i > st) return ss.substr(st, i - st);
    }
    return {};
}

LayoutFrame* build_frame_tree(SlabArena& arena,
                              const DomNode* node,
                              const CssStylesheet& sheet,
                              const ComputedStyle* parent_style,
                              float viewport_w,
                              bool light_web_default,
                              AnimationTimeline* timeline) {
    if (!node) return nullptr;

    if (node->type == NodeType::Text) {
        LayoutFrame* f = arena.create<LayoutFrame>();
        f->node = node;
        if (parent_style) f->style = *parent_style;
        f->style.display = DisplayType::Inline;
        f->style.position = PositionType::Static;
        f->style.overflow = OverflowMode::Visible;
        f->style.background = Color{0, 0, 0, 0};
        f->style.background_gradient.enabled = false;
        f->style.border_width = 0;
        f->style.border_left_width = 0;
        f->style.padding = {};
        f->style.margin = {};
        f->style.width = 0;
        f->style.height = 0;
        f->style.translate_x = 0;
        f->style.translate_y = 0;
        f->style.box_shadow.enabled = false;
        f->style.opacity = 1.0f;
        f->style.float_mode = FloatMode::None;
        f->style.clear_mode = ClearMode::None;
        f->style.width_pct = -1.0f;
        f->style.width_calc.clear();
        f->style.has_content = false;
        f->style.content_text.clear();
        return f;
    }

    ComputedStyle st = StyleEngine::compute_style(node, sheet, parent_style, viewport_w, light_web_default);

    // Session 8: primero se calcula el estilo base (sin efecto animado) para
    // conocer qué declara el elemento en `animation` / `transition`; con eso se
    // registra o actualiza su estado en la línea de tiempo y, sólo si hay algo
    // que animar, se recalcula el estilo dejando que el efecto gane la cascada.
    if (timeline) {
        const double now = timeline->clock_ms();
        timeline->register_animation(node, st.animation, now);
        if (!st.transitions.empty()) {
            timeline->register_transitions(node, st.transitions,
                                           AnimationTimeline::snapshot(st), now);
        }
        if (st.animation.valid() || !st.transitions.empty()) {
            st = StyleEngine::compute_style(node, sheet, parent_style, viewport_w,
                                            light_web_default, timeline);
        }
    }

    if (st.display == DisplayType::None) return nullptr;

    // Default styling for interactive <button> if author didn't override
    if (node->tag == "button" && st.background.a == 0) {
        if (light_web_default) {
            st.background = Color{241, 245, 249, 255};
            st.color = Color{15, 23, 42, 255};
            st.border_width = 1.0f;
            st.border_color = Color{203, 213, 225, 255};
            st.padding = {5, 10, 5, 10};
            st.border_radius = 4.0f;
        } else {
            st.background = Color{2, 132, 199, 255};
            st.color = Color{255, 255, 255, 255};
            st.padding = {8, 16, 8, 16};
            st.border_radius = 6.0f;
            st.bold = true;
        }
    }

    LayoutFrame* f = arena.create<LayoutFrame>();
    f->node = node;
    f->style = st;

    if (node->tag == "svg") return f;

    // <iframe> embedded document support (srcdoc OR pre-fetched remote HTML via data-astra-iframe-doc)
    if (node->tag == "iframe") {
        std::string srcdoc = node->get_attr("srcdoc");
        if (srcdoc.empty()) srcdoc = node->get_attr("data-astra-iframe-doc");
        if (!srcdoc.empty()) {
            static thread_local int iframe_depth = 0;
            if (iframe_depth < 2) {
                ++iframe_depth;
                HtmlDocument subdoc = HtmlDocument::parse(arena, srcdoc);
                CssStylesheet subsheet;
                subsheet.parse(subdoc.collect_stylesheets());
                const DomNode* subbody = subdoc.body() ? subdoc.body() : subdoc.root();
                if (subbody) {
                    if (LayoutFrame* subf = build_frame_tree(arena, subbody, subsheet, &f->style,
                                                            viewport_w, light_web_default, timeline)) {
                        f->children.push_back(subf);
                    }
                }
                --iframe_depth;
            }
        } else {
            std::string src = node->get_attr("src");
            DomNode* label = arena.create<DomNode>();
            label->type = NodeType::Text;
            if (src.find("youtube.com/embed/") != std::string::npos ||
                src.find("youtu.be/") != std::string::npos ||
                src.find("player.vimeo.com/") != std::string::npos) {
                label->text = "[Embedded Video Player] " + src;
            } else {
                label->text = src.empty() ? "[iframe]" : ("[iframe: " + src + "]");
            }
            LayoutFrame* lf = arena.create<LayoutFrame>();
            lf->node = label;
            lf->style = f->style;
            lf->style.display = DisplayType::Inline;
            lf->style.background = Color{0, 0, 0, 0};
            lf->style.border_width = 0;
            lf->style.padding = {};
            lf->style.margin = {};
            lf->style.color = light_web_default ? Color{71, 85, 105, 255} : Color{148, 163, 184, 255};
            lf->style.monospace = true;
            lf->style.font_size = 12.0f;
            lf->style.float_mode = FloatMode::None;
            lf->style.clear_mode = ClearMode::None;
            lf->style.width_pct = -1.0f;
            lf->style.width_calc.clear();
            lf->style.has_content = false;
            lf->style.content_text.clear();
            f->children.push_back(lf);
        }
        return f;
    }

    auto make_pseudo_frame = [&](const ComputedStyle& pst) -> LayoutFrame* {
        LayoutFrame* pf = arena.create<LayoutFrame>();
        DomNode* pnode = arena.create<DomNode>();
        pnode->type = NodeType::Element;
        pnode->tag = "span";
        DomNode* tnode = arena.create<DomNode>();
        tnode->type = NodeType::Text;
        tnode->text = pst.content_text;
        pnode->children.push_back(tnode);
        pf->node = pnode;
        pf->style = pst;

        LayoutFrame* tf = arena.create<LayoutFrame>();
        tf->node = tnode;
        tf->style = pst;
        tf->style.display = DisplayType::Inline;
        tf->style.background = Color{0, 0, 0, 0};
        tf->style.background_gradient.enabled = false;
        tf->style.border_width = 0;
        tf->style.border_left_width = 0;
        tf->style.padding = {};
        tf->style.margin = {};
        tf->style.width = 0;
        tf->style.height = 0;
        tf->style.translate_x = 0;
        tf->style.translate_y = 0;
        tf->style.box_shadow.enabled = false;
        tf->style.float_mode = FloatMode::None;
        tf->style.clear_mode = ClearMode::None;
        tf->style.width_pct = -1.0f;
        tf->style.width_calc.clear();
        tf->style.has_content = false;
        tf->style.content_text.clear();
        pf->children.push_back(tf);
        return pf;
    };

    ComputedStyle before_st;
    if (StyleEngine::compute_pseudo_style(node, sheet, "::before", f->style, before_st, viewport_w)) {
        f->children.push_back(make_pseudo_frame(before_st));
    }

    // Session 12: un nodo de texto que es sólo un espacio únicamente cuenta
    // cuando separa dos cajas EN LÍNEA. Entre cajas de bloque, CSS lo elimina,
    // así que aquí se descarta para no abrir renglones fantasma.
    auto is_collapsible_space = [](const DomNode* n) {
        return n && n->type == NodeType::Text && n->text == " ";
    };
    auto participates_inline = [&](const DomNode* n) -> bool {
        if (!n) return false;
        if (n->type == NodeType::Text) return true;
        if (n->type != NodeType::Element) return false;
        ComputedStyle cs = StyleEngine::compute_style(n, sheet, &f->style, viewport_w, light_web_default);
        return cs.display == DisplayType::Inline || cs.display == DisplayType::InlineBlock;
    };

    const std::vector<DomNode*>& kids = node->children;
    for (size_t ci = 0; ci < kids.size(); ++ci) {
        const DomNode* ch = kids[ci];
        if (is_collapsible_space(ch)) {
            const DomNode* prev = (ci > 0) ? kids[ci - 1] : nullptr;
            const DomNode* next = (ci + 1 < kids.size()) ? kids[ci + 1] : nullptr;
            // Sólo sobrevive si tiene contenido en línea a ambos lados.
            if (!prev || !next || !participates_inline(prev) || !participates_inline(next)) continue;
        }
        if (LayoutFrame* cf = build_frame_tree(arena, ch, sheet, &f->style, viewport_w,
                                               light_web_default, timeline)) {
            f->children.push_back(cf);
        }
    }

    ComputedStyle after_st;
    if (StyleEngine::compute_pseudo_style(node, sheet, "::after", f->style, after_st, viewport_w)) {
        f->children.push_back(make_pseudo_frame(after_st));
    }
    return f;
}

// `width_pre_resolved` indica que `avail_w` NO es el ancho disponible del
// bloque contenedor sino el ancho que ya se calculó para esta caja (floats,
// ítems flex, cajas en línea). Sin esta distinción, un `width: 25%` se
// aplicaba DOS veces: una al medir el ítem contra su contenedor y otra aquí
// sobre el resultado, dejando las columnas a un tercio de su tamaño.
void layout_node(LayoutFrame* frame, float x, float y, float avail_w,
                 const Rect& containing_block, const Rect& viewport,
                 bool width_pre_resolved = false) {
    if (!frame) return;
    const ComputedStyle& st = frame->style;

    float box_x = x + st.margin.left;
    float box_y = y + st.margin.top;
    float pad_x = st.padding.left + st.border_width;
    float pad_y = st.padding.top + st.border_width;
    float horiz_extra = pad_x + st.padding.right + st.border_width;
    float vert_extra = pad_y + st.padding.bottom + st.border_width;
    // El suelo de 40px evita cajas de bloque degeneradas, pero NO debe
    // aplicarse a las cajas en línea: una palabra corta o un signo de
    // puntuación miden mucho menos, y con el suelo cada uno abría un hueco
    // artificial dentro de la línea de texto.
    const float avail_minus_margins = avail_w - st.margin.left - st.margin.right;
    float max_avail = (st.display == DisplayType::Inline)
                          ? std::max(0.0f, avail_minus_margins)
                          : std::max(40.0f, avail_minus_margins);

    float box_w = max_avail;
    float resolved_width = st.width;
    if (width_pre_resolved) {
        // El llamante ya resolvió el porcentaje o el calc() contra el bloque
        // contenedor: `avail_w` es el ancho final de esta caja.
        if (st.width_pct >= 0.0f || !st.width_calc.empty()) {
            resolved_width = (st.box_sizing == BoxSizing::BorderBox)
                                 ? max_avail
                                 : std::max(0.0f, max_avail - horiz_extra);
        }
    } else if (!st.width_calc.empty()) {
        resolved_width = StyleEngine::eval_calc(st.width_calc, max_avail, viewport.w, -1.0f);
    } else if (st.width_pct >= 0.0f) {
        resolved_width = max_avail * st.width_pct * 0.01f;
    }
    if (resolved_width > 0.0f) {
        float target_border_w = (st.box_sizing == BoxSizing::BorderBox)
                                    ? resolved_width
                                    : (resolved_width + horiz_extra);
        box_w = std::min(target_border_w, max_avail);
    }
    if (st.max_width > 0.0f) {
        float max_bw = (st.box_sizing == BoxSizing::BorderBox) ? st.max_width : (st.max_width + horiz_extra);
        box_w = std::min(box_w, max_bw);
    }
    if (st.min_width > 0.0f) {
        float min_bw = (st.box_sizing == BoxSizing::BorderBox) ? st.min_width : (st.min_width + horiz_extra);
        box_w = std::min(max_avail, std::max(box_w, min_bw));
    }

    float inner_w = std::max(16.0f, box_w - horiz_extra);

    frame->border_box.x = box_x;
    frame->border_box.y = box_y;
    frame->border_box.w = box_w;

    frame->content_box.x = box_x + pad_x;
    frame->content_box.y = box_y + pad_y;
    frame->content_box.w = inner_w;

    // Replaced / embedded elements (<astra-3d>, <video>, <audio>, <canvas>, <img>, <input>)
    if (frame->node && frame->node->type == NodeType::Element) {
        const std::string& tag = frame->node->tag;
        if (tag == "astra-3d" || tag == "video") {
            float h = (st.height > 0.0f) ? st.height : 260.0f;
            frame->content_box.h = h;
            frame->border_box.h = h + vert_extra;
            return;
        }
        if (tag == "audio") {
            float h = (st.height > 0.0f) ? st.height : 76.0f;
            frame->content_box.h = h;
            frame->border_box.h = h + vert_extra;
            return;
        }
        if (tag == "canvas") {
            float aw = static_cast<float>(std::atoi(frame->node->get_attr("width").c_str()));
            float ah = static_cast<float>(std::atoi(frame->node->get_attr("height").c_str()));
            float w = (st.width > 0.0f) ? st.width : (aw > 0.0f ? aw : std::min(inner_w, 300.0f));
            float h = (st.height > 0.0f) ? st.height : (ah > 0.0f ? ah : 150.0f);
            frame->content_box.w = std::min(w, inner_w);
            frame->content_box.h = h;
            frame->border_box.w = frame->content_box.w + horiz_extra;
            frame->border_box.h = h + vert_extra;
            return;
        }
        if (tag == "img") {
            float aw = static_cast<float>(std::atoi(frame->node->get_attr("width").c_str()));
            float ah = static_cast<float>(std::atoi(frame->node->get_attr("height").c_str()));
            float w = (st.width > 0.0f) ? st.width : (aw > 0.0f ? aw : (ah > 0.0f ? ah : std::min(inner_w, 220.0f)));
            float h = (st.height > 0.0f) ? st.height : (ah > 0.0f ? ah : (aw > 0.0f ? std::min(aw, 220.0f) : 140.0f));
            frame->content_box.w = std::min(w, inner_w);
            frame->content_box.h = h;
            frame->border_box.w = frame->content_box.w + horiz_extra;
            frame->border_box.h = h + vert_extra;
            return;
        }
        if (tag == "svg") {
            float aw = static_cast<float>(std::atoi(frame->node->get_attr("width").c_str()));
            float ah = static_cast<float>(std::atoi(frame->node->get_attr("height").c_str()));
            float w = (st.width > 0.0f) ? st.width : (aw > 0.0f ? aw : 24.0f);
            float h = (st.height > 0.0f) ? st.height : (ah > 0.0f ? ah : 24.0f);
            frame->content_box.w = std::min(w, inner_w);
            frame->content_box.h = h;
            frame->border_box.w = frame->content_box.w + horiz_extra;
            frame->border_box.h = h + vert_extra;
            return;
        }
        if (tag == "br") {
            float lh = line_height_for_size(st.font_size);
            frame->content_box.w = 0.0f;
            frame->content_box.h = lh * 0.5f;
            frame->border_box.w = 0.0f;
            frame->border_box.h = lh * 0.5f;
            return;
        }
        if (tag == "input" || tag == "textarea") {
            float h = (st.height > 0.0f) ? st.height : (tag == "textarea" ? 88.0f : 38.0f);
            frame->border_box.h = h;
            frame->content_box.h = std::max(16.0f, h - vert_extra);
            return;
        }
    }

    // Text frame
    if (frame->node && frame->node->type == NodeType::Text) {
        bool keep_lead = false, keep_trail = false;
        if (const DomNode* tn = frame->node; tn && tn->parent) {
            const auto& sib = tn->parent->children;
            for (size_t k = 0; k < sib.size(); ++k) {
                if (sib[k] == tn) {
                    keep_lead = (k > 0);
                    keep_trail = (k + 1 < sib.size());
                    break;
                }
            }
        }
        frame->wrapped_lines = wrap_text(frame->node->text, inner_w, st.font_size, st.bold, st.monospace,
                                         keep_lead, keep_trail);
        float lh = line_height_for_size(st.font_size);
        frame->content_box.h = static_cast<float>(frame->wrapped_lines.size()) * lh;
        frame->border_box.h = frame->content_box.h;
        return;
    }

    // Split normal-flow children vs out-of-flow (absolute/fixed) children
    std::vector<LayoutFrame*> flow_children;
    std::vector<LayoutFrame*> positioned_children;
    flow_children.reserve(frame->children.size());
    for (LayoutFrame* ch : frame->children) {
        if (ch->style.position == PositionType::Absolute || ch->style.position == PositionType::Fixed) {
            positioned_children.push_back(ch);
        } else {
            flow_children.push_back(ch);
        }
    }

    Rect next_cb = (st.position != PositionType::Static) ? frame->border_box : containing_block;
    float cur_y = frame->content_box.y;

    if (st.display == DisplayType::Table && !flow_children.empty()) {
        // Collect all <tr> rows (flattening <thead>/<tbody>/<tfoot> wrappers)
        std::vector<LayoutFrame*> rows;
        std::function<void(LayoutFrame*)> collect_rows = [&](LayoutFrame* n) {
            if (!n) return;
            if (n->style.display == DisplayType::TableRow || (n->node && n->node->tag == "tr")) {
                rows.push_back(n);
                return;
            }
            for (LayoutFrame* c : n->children) collect_rows(c);
        };
        for (LayoutFrame* ch : flow_children) collect_rows(ch);

        // Determine column count and preferred column widths (clamped against hostile colspan)
        size_t ncols = 0;
        for (LayoutFrame* r : rows) {
            size_t cols_in_row = 0;
            for (LayoutFrame* cell : r->children) {
                int span = cell->node ? std::clamp(std::atoi(cell->node->get_attr("colspan").c_str()), 1, 64) : 1;
                cols_in_row += static_cast<size_t>(span);
            }
            ncols = std::max(ncols, cols_in_row);
        }
        ncols = std::min<size_t>(ncols, 64);
        if (ncols > 0) {
            std::vector<float> col_pref(ncols, 60.0f);
            for (LayoutFrame* r : rows) {
                size_t ci = 0;
                for (LayoutFrame* cell : r->children) {
                    int span = cell->node ? std::clamp(std::atoi(cell->node->get_attr("colspan").c_str()), 1, 64) : 1;
                    if (span == 1 && ci < ncols) {
                        const ComputedStyle& cs = cell->style;
                        float extra = cs.padding.left + cs.padding.right + cs.border_width * 2.0f;
                        if (cs.width > 0.0f) {
                            col_pref[ci] = std::max(col_pref[ci], cs.width + extra);
                        } else if (cell->node) {
                            float tw = FontEngine::instance().measure_text(
                                cell->node->text_content(), cs.font_size, cs.bold, cs.monospace);
                            col_pref[ci] = std::max(col_pref[ci], std::min(inner_w * 0.6f, tw + extra + 14.0f));
                        }
                    }
                    ci += static_cast<size_t>(span);
                }
            }
            float sum_pref = 0.0f;
            for (float w : col_pref) sum_pref += w;
            std::vector<float> col_w(ncols, inner_w / static_cast<float>(ncols));
            if (sum_pref > 1.0f) {
                for (size_t c = 0; c < ncols; ++c) {
                    col_w[c] = std::max(40.0f, inner_w * (col_pref[c] / sum_pref));
                }
            }
            std::vector<float> col_x(ncols, frame->content_box.x);
            float cx = frame->content_box.x;
            for (size_t c = 0; c < ncols; ++c) {
                col_x[c] = cx;
                cx += col_w[c];
            }

            for (LayoutFrame* r : rows) {
                r->border_box.x = frame->content_box.x;
                r->border_box.y = cur_y;
                r->border_box.w = inner_w;
                r->content_box = r->border_box;
                size_t ci = 0;
                float row_h = 28.0f;
                for (LayoutFrame* cell : r->children) {
                    if (ci >= ncols) break;
                    int span = cell->node ? std::clamp(std::atoi(cell->node->get_attr("colspan").c_str()), 1, 64) : 1;
                    float cw = 0.0f;
                    for (int s = 0; s < span && (ci + static_cast<size_t>(s)) < ncols; ++s) {
                        cw += col_w[ci + static_cast<size_t>(s)];
                    }
                    layout_node(cell, col_x[ci], cur_y, cw, next_cb, viewport);
                    row_h = std::max(row_h, cell->border_box.h);
                    ci += static_cast<size_t>(span);
                }
                // Equalize all cells in the row to uniform row_h
                for (LayoutFrame* cell : r->children) {
                    float dh = row_h - cell->border_box.h;
                    if (dh > 0.0f) {
                        cell->border_box.h = row_h;
                        cell->content_box.h += dh;
                    }
                }
                r->border_box.h = row_h;
                r->content_box.h = row_h;
                cur_y += row_h;
            }
        }
    } else if (st.display == DisplayType::Grid && !flow_children.empty()) {
        std::vector<GridTrack> cols = st.grid_columns;
        if (cols.empty()) cols = {GridTrack{1.0f, true}, GridTrack{1.0f, true}};
        size_t ncols = cols.size();
        // Separación por eje: `column-gap`/`row-gap` mandan sobre `gap`.
        const float cgap = (st.column_gap >= 0.0f) ? st.column_gap : st.gap;
        const float rgap = (st.row_gap >= 0.0f) ? st.row_gap : st.gap;
        float total_gap = cgap * static_cast<float>(ncols > 1 ? ncols - 1 : 0);
        float fixed_px = 0.0f;
        float total_fr = 0.0f;
        for (const auto& t : cols) {
            if (t.is_fr) total_fr += t.value;
            else fixed_px += t.value;
        }
        float rem_w = std::max(0.0f, inner_w - total_gap - fixed_px);
        std::vector<float> col_w(ncols, 60.0f);
        std::vector<float> col_x(ncols, frame->content_box.x);
        float cx = frame->content_box.x;
        for (size_t c = 0; c < ncols; ++c) {
            col_w[c] = cols[c].is_fr
                           ? std::max(40.0f, (total_fr > 0.0f ? rem_w * (cols[c].value / total_fr) : 60.0f))
                           : cols[c].value;
            col_x[c] = cx;
            cx += col_w[c] + cgap;
        }
        // CSS Grid Level 1 §7.3 — colocación por áreas con nombre.
        // Cuando el contenedor declara `grid-template-areas`, cada hijo con
        // `grid-area: <nombre>` se coloca en el rectángulo de pistas que ocupa
        // ese nombre en la plantilla, en vez de fluir secuencialmente. Es lo
        // que permite que una barra lateral y el contenido queden en columnas
        // contiguas en lugar de apilarse uno encima del otro.
        bool placed_by_area = false;
        if (!st.grid_areas.empty()) {
            struct AreaRect { size_t r0, r1, c0, c1; };
            std::map<std::string, AreaRect> areas;
            for (size_t r = 0; r < st.grid_areas.size(); ++r) {
                const auto& row = st.grid_areas[r];
                for (size_t c = 0; c < row.size() && c < ncols; ++c) {
                    const std::string& nm = row[c];
                    if (nm.empty() || nm == ".") continue; // celda vacía
                    auto it = areas.find(nm);
                    if (it == areas.end()) {
                        areas.emplace(nm, AreaRect{r, r, c, c});
                    } else {
                        it->second.r0 = std::min(it->second.r0, r);
                        it->second.r1 = std::max(it->second.r1, r);
                        it->second.c0 = std::min(it->second.c0, c);
                        it->second.c1 = std::max(it->second.c1, c);
                    }
                }
            }

            // Reparte los hijos por fila de la plantilla; los que no nombran un
            // área conocida se dejan para el flujo automático posterior.
            std::vector<std::vector<LayoutFrame*>> rows(st.grid_areas.size());
            std::vector<LayoutFrame*> unplaced;
            for (LayoutFrame* ch : flow_children) {
                auto it = ch->style.grid_area.empty() ? areas.end() : areas.find(ch->style.grid_area);
                if (it == areas.end()) {
                    unplaced.push_back(ch);
                } else {
                    rows[it->second.r0].push_back(ch);
                }
            }

            if (rows.size() > 0) {
                for (size_t r = 0; r < rows.size(); ++r) {
                    if (rows[r].empty()) continue;
                    float row_h = 0.0f;
                    for (LayoutFrame* ch : rows[r]) {
                        const AreaRect& ar = areas[ch->style.grid_area];
                        const size_t c0 = std::min(ar.c0, ncols - 1);
                        const size_t c1 = std::min(ar.c1, ncols - 1);
                        // Ancho del área = pistas abarcadas + los gaps interiores.
                        float span_w = 0.0f;
                        for (size_t c = c0; c <= c1; ++c) span_w += col_w[c];
                        span_w += cgap * static_cast<float>(c1 - c0);
                        layout_node(ch, col_x[c0], cur_y, span_w, next_cb, viewport);
                        float ch_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                        row_h = std::max(row_h, ch_h);
                    }
                    cur_y += row_h + rgap;
                }
                if (!rows.empty()) cur_y -= rgap;
                placed_by_area = true;
            }

            // Los hijos sin área declarada siguen en flujo automático debajo.
            flow_children.swap(unplaced);
            if (!flow_children.empty() && placed_by_area) cur_y += rgap;
        }

        size_t idx = 0;
        while (idx < flow_children.size()) {
            float row_h = 0.0f;
            for (size_t c = 0; c < ncols && idx < flow_children.size(); ++c, ++idx) {
                LayoutFrame* ch = flow_children[idx];
                layout_node(ch, col_x[c], cur_y, col_w[c], next_cb, viewport);
                float ch_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                row_h = std::max(row_h, ch_h);
            }
            cur_y += row_h;
            if (idx < flow_children.size()) cur_y += rgap;
        }
    } else if (st.display == DisplayType::Flex && st.flex_direction == FlexDirection::Row && !flow_children.empty()) {
        auto intrinsic_html_w = [](const LayoutFrame* f) -> float {
            if (!f || !f->node || f->node->type != NodeType::Element) return 0.0f;
            if (f->node->tag == "svg" || f->node->tag == "img" || f->node->tag == "canvas") {
                std::string wa = f->node->get_attr("width");
                if (!wa.empty()) {
                    float wv = std::strtof(wa.c_str(), nullptr);
                    if (wv > 0.0f) return wv;
                }
            }
            return 0.0f;
        };

        if (st.flex_wrap) {
            // Multi-row flex-wrap layout
            std::vector<float> item_w(flow_children.size(), 0.0f);
            for (size_t i = 0; i < flow_children.size(); ++i) {
                const ComputedStyle& cs = flow_children[i]->style;
                float extra = cs.padding.left + cs.padding.right + cs.border_width * 2.0f;
                float attr_w = intrinsic_html_w(flow_children[i]);
                if (cs.flex_basis >= 0.0f) {
                    float bw = (cs.box_sizing == BoxSizing::BorderBox) ? cs.flex_basis : (cs.flex_basis + extra);
                    item_w[i] = std::min(inner_w, bw + cs.margin.left + cs.margin.right);
                } else if (cs.width_pct >= 0.0f || !cs.width_calc.empty()) {
                    // CSS Flexbox §9.2: un ancho en porcentaje (o calc) de un
                    // ítem se resuelve contra el tamaño interior del
                    // contenedor. Sin esta rama caía al respaldo fijo y las
                    // rejillas de 12 columnas quedaban en ~160px.
                    float pw = !cs.width_calc.empty()
                                   ? StyleEngine::eval_calc(cs.width_calc, inner_w, viewport.w, -1.0f)
                                   : inner_w * cs.width_pct * 0.01f;
                    if (pw < 0.0f) pw = inner_w * 0.5f;
                    float bw = (cs.box_sizing == BoxSizing::BorderBox) ? pw : (pw + extra);
                    item_w[i] = std::min(inner_w, bw + cs.margin.left + cs.margin.right);
                } else if (cs.width > 0.0f) {
                    float bw = (cs.box_sizing == BoxSizing::BorderBox) ? cs.width : (cs.width + extra);
                    item_w[i] = std::min(inner_w, bw + cs.margin.left + cs.margin.right);
                } else if (attr_w > 0.0f) {
                    item_w[i] = std::min(inner_w, attr_w + extra + cs.margin.left + cs.margin.right);
                } else if (flow_children[i]->children.empty() &&
                           flow_children[i]->node && !flow_children[i]->node->text_content().empty()) {
                    float tw = FontEngine::instance().measure_text(
                        flow_children[i]->node->text_content(), cs.font_size, cs.bold, cs.monospace);
                    item_w[i] = std::min(inner_w, tw + extra + cs.margin.left + cs.margin.right + 16.0f);
                } else if (cs.flex_grow > 0.0f) {
                    // Un ítem elástico parte de cero: su tamaño sale del
                    // reparto del espacio libre, no de un valor arbitrario.
                    item_w[i] = 0.0f;
                } else {
                    item_w[i] = std::min(inner_w, 160.0f);
                }
            }

            size_t idx = 0;
            bool first_row = true;
            while (idx < flow_children.size()) {
                if (!first_row) cur_y += st.gap;
                first_row = false;

                size_t row_start = idx;
                float row_used = 0.0f;
                while (idx < flow_children.size()) {
                    float add = item_w[idx] + (idx > row_start ? st.gap : 0.0f);
                    if (idx > row_start && row_used + add > inner_w + 0.5f) break;
                    row_used += add;
                    ++idx;
                }
                size_t row_cnt = idx - row_start;
                float free_sp = std::max(0.0f, inner_w - row_used);

                // CSS Flexbox §9.7: antes de alinear la fila, los ítems con
                // `flex-grow` se reparten el espacio libre que quede en ella.
                // Sin esto, una barra con un bloque fijo y otro elástico
                // dejaba ambos pegados a la izquierda.
                if (free_sp > 0.5f) {
                    float sum_grow = 0.0f;
                    for (size_t j = row_start; j < idx; ++j) sum_grow += flow_children[j]->style.flex_grow;
                    if (sum_grow > 0.0f) {
                        for (size_t j = row_start; j < idx; ++j) {
                            const float g = flow_children[j]->style.flex_grow;
                            if (g > 0.0f) item_w[j] += free_sp * (g / sum_grow);
                        }
                        row_used += free_sp;
                        free_sp = 0.0f;
                    }
                }

                float start_x = frame->content_box.x;
                float gap_x = st.gap;
                if (st.justify_content == JustifyContent::Center) start_x += free_sp * 0.5f;
                else if (st.justify_content == JustifyContent::FlexEnd) start_x += free_sp;
                else if (st.justify_content == JustifyContent::SpaceBetween && row_cnt > 1) {
                    gap_x = st.gap + free_sp / static_cast<float>(row_cnt - 1);
                }

                float cur_x = start_x;
                float row_h = 0.0f;
                for (size_t j = row_start; j < idx; ++j) {
                    LayoutFrame* ch = flow_children[j];
                    layout_node(ch, cur_x, cur_y, item_w[j], next_cb, viewport, true);
                    float ch_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                    row_h = std::max(row_h, ch_h);
                    cur_x += item_w[j] + gap_x;
                }
                if (st.align_items == AlignItems::Center || st.align_items == AlignItems::FlexEnd) {
                    for (size_t j = row_start; j < idx; ++j) {
                        LayoutFrame* ch = flow_children[j];
                        float ch_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                        float diff = std::max(0.0f, row_h - ch_h);
                        if (diff > 0.5f) {
                            offset_subtree(ch, 0.0f, (st.align_items == AlignItems::Center) ? (diff * 0.5f) : diff);
                        }
                    }
                }
                cur_y += row_h;
            }
        } else {
        size_t count = flow_children.size();
        float total_gap = st.gap * static_cast<float>(count > 0 ? count - 1 : 0);
        bool has_explicit_w = false;
        bool has_flex_grow = false;
        for (LayoutFrame* ch : flow_children) {
            if (ch->style.flex_grow > 0.0f) has_flex_grow = true;
            if (ch->style.width > 0.0f || ch->style.flex_basis >= 0.0f ||
                ch->style.width_pct >= 0.0f || !ch->style.width_calc.empty() ||
                intrinsic_html_w(ch) > 0.0f) {
                has_explicit_w = true;
            }
        }
        // Con `flex-grow` el reparto deja de ser equitativo, así que se toma
        // siempre la rama que mide cada ítem y distribuye el sobrante.
        bool custom_justify = (st.justify_content != JustifyContent::FlexStart) || has_explicit_w || has_flex_grow;
        float max_h = 0.0f;

        if (!custom_justify) {
            float item_w = std::max(40.0f, (inner_w - total_gap) / static_cast<float>(count));
            float cur_x = frame->content_box.x;
            for (LayoutFrame* ch : flow_children) {
                layout_node(ch, cur_x, cur_y, item_w, next_cb, viewport);
                float ch_total_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                max_h = std::max(max_h, ch_total_h);
                cur_x += item_w + st.gap;
            }
        } else {
            // Measure each child's intrinsic or explicit width
            std::vector<float> child_w(count, 0.0f);
            float used_w = 0.0f;
            size_t auto_count = 0;
            for (size_t i = 0; i < count; ++i) {
                const ComputedStyle& cs = flow_children[i]->style;
                float extra = cs.padding.left + cs.padding.right + cs.border_width * 2.0f;
                float attr_w = intrinsic_html_w(flow_children[i]);
                if (cs.flex_basis >= 0.0f) {
                    float bw = (cs.box_sizing == BoxSizing::BorderBox) ? cs.flex_basis : (cs.flex_basis + extra);
                    child_w[i] = bw + cs.margin.left + cs.margin.right;
                    used_w += child_w[i];
                } else if (cs.width_pct >= 0.0f || !cs.width_calc.empty()) {
                    float pw = !cs.width_calc.empty()
                                   ? StyleEngine::eval_calc(cs.width_calc, inner_w, viewport.w, -1.0f)
                                   : inner_w * cs.width_pct * 0.01f;
                    if (pw < 0.0f) pw = inner_w * 0.5f;
                    float bw = (cs.box_sizing == BoxSizing::BorderBox) ? pw : (pw + extra);
                    child_w[i] = bw + cs.margin.left + cs.margin.right;
                    used_w += child_w[i];
                } else if (cs.width > 0.0f) {
                    float bw = (cs.box_sizing == BoxSizing::BorderBox) ? cs.width : (cs.width + extra);
                    child_w[i] = bw + cs.margin.left + cs.margin.right;
                    used_w += child_w[i];
                } else if (attr_w > 0.0f) {
                    child_w[i] = attr_w + extra + cs.margin.left + cs.margin.right;
                    used_w += child_w[i];
                } else if (st.justify_content != JustifyContent::FlexStart &&
                           flow_children[i]->children.empty() &&
                           flow_children[i]->node && !flow_children[i]->node->text_content().empty()) {
                    float tw = FontEngine::instance().measure_text(
                        flow_children[i]->node->text_content(), cs.font_size, cs.bold, cs.monospace);
                    child_w[i] = std::min(inner_w, tw + extra + cs.margin.left + cs.margin.right + 12.0f);
                    used_w += child_w[i];
                } else {
                    ++auto_count;
                }
            }
            if (auto_count > 0) {
                float rem = std::max(40.0f * static_cast<float>(auto_count), inner_w - total_gap - used_w);
                float share = rem / static_cast<float>(auto_count);
                for (size_t i = 0; i < count; ++i) {
                    if (child_w[i] <= 0.0f) {
                        child_w[i] = share;
                        used_w += share;
                    }
                }
            }
            float avail_items_w = std::max(40.0f, inner_w - total_gap);

            // CSS Flexbox §9.7: el espacio libre sobrante se reparte entre los
            // ítems en proporción a su `flex-grow`. Es lo que empuja un bloque
            // contra el borde opuesto cuando su hermano declara `flex-grow: 1`.
            if (has_flex_grow && used_w < avail_items_w) {
                float sum_grow = 0.0f;
                for (size_t i = 0; i < count; ++i) sum_grow += flow_children[i]->style.flex_grow;
                if (sum_grow > 0.0f) {
                    const float free_w = avail_items_w - used_w;
                    for (size_t i = 0; i < count; ++i) {
                        const float g = flow_children[i]->style.flex_grow;
                        if (g <= 0.0f) continue;
                        const float add = free_w * (g / sum_grow);
                        child_w[i] += add;
                        used_w += add;
                    }
                }
            }

            if (used_w > avail_items_w && used_w > 0.0f) {
                float scale = avail_items_w / used_w;
                used_w = 0.0f;
                for (size_t i = 0; i < count; ++i) {
                    child_w[i] *= scale;
                    used_w += child_w[i];
                }
            }

            float free_space = std::max(0.0f, inner_w - used_w - total_gap);
            float start_x = frame->content_box.x;
            float item_gap = st.gap;
            if (st.justify_content == JustifyContent::Center) {
                start_x += free_space * 0.5f;
            } else if (st.justify_content == JustifyContent::FlexEnd) {
                start_x += free_space;
            } else if (st.justify_content == JustifyContent::SpaceBetween && count > 1) {
                item_gap = st.gap + free_space / static_cast<float>(count - 1);
            }

            float cur_x = start_x;
            for (size_t i = 0; i < count; ++i) {
                LayoutFrame* ch = flow_children[i];
                layout_node(ch, cur_x, cur_y, child_w[i], next_cb, viewport, true);
                float ch_total_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                max_h = std::max(max_h, ch_total_h);
                cur_x += child_w[i] + item_gap;
            }
        }

        // Cross-axis align-items adjustment (respecting explicit container height)
        float cross_h = max_h;
        if (st.height > 0.0f) {
            float target_h = (st.box_sizing == BoxSizing::BorderBox)
                                 ? std::max(0.0f, st.height - vert_extra)
                                 : st.height;
            cross_h = std::max(cross_h, target_h);
        }
        if (st.align_items == AlignItems::Center || st.align_items == AlignItems::FlexEnd) {
            for (LayoutFrame* ch : flow_children) {
                float ch_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
                float diff = std::max(0.0f, cross_h - ch_h);
                if (diff > 0.5f) {
                    float dy = (st.align_items == AlignItems::Center) ? (diff * 0.5f) : diff;
                    offset_subtree(ch, 0.0f, dy);
                }
            }
        }
        cur_y += max_h;
        }
    } else if (st.display == DisplayType::Flex && st.flex_direction == FlexDirection::Column) {
        bool first = true;
        for (LayoutFrame* ch : flow_children) {
            if (!first) cur_y += st.gap;
            first = false;
            layout_node(ch, frame->content_box.x, cur_y, inner_w, next_cb, viewport);
            cur_y = ch->border_box.y + ch->border_box.h + ch->style.margin.bottom;
        }
    } else {
        // Standard Block Formatting Context with Float/Clear + Real Inline Formatting Context (IFC)
        float left_float_w = 0.0f;
        float left_float_end_y = cur_y;
        float right_float_w = 0.0f;
        float right_float_end_y = cur_y;

        auto measure_inline_w = [&](const LayoutFrame* ch, float avail) -> float {
            const ComputedStyle& cs = ch->style;
            float extra = cs.padding.left + cs.padding.right + cs.border_width * 2.0f;
            float rw = cs.width;
            if (!cs.width_calc.empty()) rw = StyleEngine::eval_calc(cs.width_calc, avail, viewport.w, -1.0f);
            else if (cs.width_pct >= 0.0f) rw = avail * cs.width_pct * 0.01f;
            if (rw > 0.0f) {
                float bw = (cs.box_sizing == BoxSizing::BorderBox) ? rw : (rw + extra);
                return std::min(avail, bw + cs.margin.left + cs.margin.right);
            }
            if (ch->node && ch->node->type == NodeType::Element) {
                if (ch->node->tag == "img" || ch->node->tag == "svg") {
                    float aw = static_cast<float>(std::atoi(ch->node->get_attr("width").c_str()));
                    float ah = static_cast<float>(std::atoi(ch->node->get_attr("height").c_str()));
                    float iw = (aw > 0.0f) ? aw : (ah > 0.0f ? ah : (ch->node->tag == "svg" ? 24.0f : 180.0f));
                    return std::min(avail, iw + extra + cs.margin.left + cs.margin.right);
                }
            }
            std::string txt = ch->node ? ch->node->text_content() : "";
            if (txt.empty()) {
                return std::min(avail, std::max(8.0f, extra + cs.margin.left + cs.margin.right));
            }
            float tw = FontEngine::instance().measure_text(txt, cs.font_size, cs.bold, cs.monospace);
            float pad_fudge = (cs.display == DisplayType::InlineBlock) ? 8.0f : 2.0f;
            return std::min(avail, tw + extra + cs.margin.left + cs.margin.right + pad_fudge);
        };

        auto is_inline_flow = [](const LayoutFrame* ch) -> bool {
            if (!ch || ch->style.float_mode != FloatMode::None) return false;
            if (ch->node && ch->node->type == NodeType::Element && ch->node->tag == "br") return false;
            return ch->style.display == DisplayType::InlineBlock ||
                   ch->style.display == DisplayType::Inline;
        };

        size_t i = 0;
        while (i < flow_children.size()) {
            LayoutFrame* ch = flow_children[i];
            const ComputedStyle& cs = ch->style;

            // Expire floats we have scrolled past
            if (cur_y >= left_float_end_y) left_float_w = 0.0f;
            if (cur_y >= right_float_end_y) right_float_w = 0.0f;

            // Handle CSS clear: left | right | both
            if (cs.clear_mode == ClearMode::Left || cs.clear_mode == ClearMode::Both) {
                if (cur_y < left_float_end_y) cur_y = left_float_end_y;
                left_float_w = 0.0f;
            }
            if (cs.clear_mode == ClearMode::Right || cs.clear_mode == ClearMode::Both) {
                if (cur_y < right_float_end_y) cur_y = right_float_end_y;
                right_float_w = 0.0f;
            }

            // Handle CSS float: left | right
            if (cs.float_mode == FloatMode::Left || cs.float_mode == FloatMode::Right) {
                float fw = measure_inline_w(ch, inner_w);
                float fx = (cs.float_mode == FloatMode::Left)
                               ? (frame->content_box.x + left_float_w)
                               : (frame->content_box.x + inner_w - right_float_w - fw);
                layout_node(ch, fx, cur_y, fw, next_cb, viewport, true);
                float end_y = ch->border_box.y + ch->border_box.h + cs.margin.bottom;
                float used_w = ch->border_box.w + cs.margin.left + cs.margin.right;
                if (cs.float_mode == FloatMode::Left) {
                    left_float_w += used_w;
                    left_float_end_y = std::max(left_float_end_y, end_y);
                } else {
                    right_float_w += used_w;
                    right_float_end_y = std::max(right_float_end_y, end_y);
                }
                ++i;
                continue;
            }

            // Compute horizontal span excluding active floats at cur_y
            float avail_x = frame->content_box.x + (cur_y < left_float_end_y ? left_float_w : 0.0f);
            float excl_r = (cur_y < right_float_end_y ? right_float_w : 0.0f);
            float avail_line_w = std::max(60.0f, inner_w - (avail_x - frame->content_box.x) - excl_r);

            // Real Inline Formatting Context (IFC): lay out consecutive inline / inline-block / inline-img runs horizontally with wrapping
            if (is_inline_flow(ch)) {
                size_t run_end = i;
                bool run_has_inline_block = false;
                while (run_end < flow_children.size() && is_inline_flow(flow_children[run_end])) {
                    if (flow_children[run_end]->style.display == DisplayType::InlineBlock) {
                        run_has_inline_block = true;
                    }
                    ++run_end;
                }
                if (run_end - i > 1 || run_has_inline_block) {
                    float line_x = avail_x;
                    float line_right = avail_x + avail_line_w;
                    float line_h = 0.0f;
                    std::vector<LayoutFrame*> line_items;

                    auto flush_line_align = [&]() {
                        if (line_items.size() > 1 && line_h > 0.0f) {
                            for (LayoutFrame* li : line_items) {
                                float ih = li->border_box.h + li->style.margin.top + li->style.margin.bottom;
                                float diff = line_h - ih;
                                if (diff > 2.0f) {
                                    offset_subtree(li, 0.0f, diff * 0.5f);
                                }
                            }
                        }
                        line_items.clear();
                    };

                    while (i < run_end) {
                        LayoutFrame* ich = flow_children[i];
                        float iw = measure_inline_w(ich, avail_line_w);
                        if (line_x > avail_x && line_x + iw > line_right + 0.5f) {
                            flush_line_align();
                            cur_y += line_h + 2.0f;
                            line_h = 0.0f;
                            avail_x = frame->content_box.x + (cur_y < left_float_end_y ? left_float_w : 0.0f);
                            excl_r = (cur_y < right_float_end_y ? right_float_w : 0.0f);
                            avail_line_w = std::max(60.0f, inner_w - (avail_x - frame->content_box.x) - excl_r);
                            line_x = avail_x;
                            line_right = avail_x + avail_line_w;
                            iw = measure_inline_w(ich, avail_line_w);
                        }
                        layout_node(ich, line_x, cur_y, iw, next_cb, viewport, true);

                        // CSS Text §4.1.1: el espacio que queda al PRINCIPIO de
                        // una línea se elimina. Conservarlo (necesario entre
                        // cajas) dejaba una sangría de unos pocos píxeles en
                        // cada renglón que empezaba con un nodo de texto.
                        if (line_x <= avail_x + 0.5f && !ich->wrapped_lines.empty() &&
                            !ich->wrapped_lines.front().empty() &&
                            ich->wrapped_lines.front().front() == ' ') {
                            const float space_w = FontEngine::instance().measure_text(
                                " ", ich->style.font_size, ich->style.bold, ich->style.monospace);
                            ich->wrapped_lines.front().erase(ich->wrapped_lines.front().begin());
                            ich->border_box.w = std::max(0.0f, ich->border_box.w - space_w);
                            ich->content_box.w = std::max(0.0f, ich->content_box.w - space_w);
                        }

                        float ih = ich->border_box.h + ich->style.margin.top + ich->style.margin.bottom;
                        line_h = std::max(line_h, ih);
                        line_items.push_back(ich);
                        // Las cajas en línea se colocan contiguas: el espacio
                        // entre palabras ya viene en el propio texto. Sólo los
                        // inline-block conservan una holgura, porque el nodo de
                        // texto en blanco que los separaba se descarta al parsear.
                        float inline_gap = (ich->style.display == DisplayType::InlineBlock) ? 4.0f : 0.0f;
                        line_x += ich->border_box.w + ich->style.margin.left + ich->style.margin.right + inline_gap;
                        ++i;
                    }
                    flush_line_align();
                    cur_y += line_h;
                    continue;
                }
            }

            // Regular block child (or single inline text child filling available width)
            bool explicit_w = (cs.width > 0.0f || cs.width_pct >= 0.0f || !cs.width_calc.empty());
            float block_avail_w = explicit_w ? inner_w : avail_line_w;
            layout_node(ch, avail_x, cur_y, block_avail_w, next_cb, viewport);
            cur_y = ch->border_box.y + ch->border_box.h + cs.margin.bottom;
            ++i;
        }
        // Ensure container clears internal floats
        cur_y = std::max(cur_y, std::max(left_float_end_y, right_float_end_y));
    }

    float measured_h = std::max(0.0f, cur_y - frame->content_box.y);
    if (st.height > 0.0f) {
        float target_content_h = (st.box_sizing == BoxSizing::BorderBox)
                                     ? std::max(0.0f, st.height - vert_extra)
                                     : st.height;
        if (st.overflow == OverflowMode::Hidden) {
            measured_h = target_content_h;
        } else {
            measured_h = std::max(measured_h, target_content_h);
        }
    }
    frame->content_box.h = measured_h;
    frame->border_box.h = measured_h + vert_extra;

    // Update containing block height now that this container's final height is known
    Rect final_cb = (st.position != PositionType::Static) ? frame->border_box : containing_block;

    // Layout out-of-flow absolute / fixed children against their containing block or viewport
    for (LayoutFrame* ch : positioned_children) {
        const ComputedStyle& cst = ch->style;
        Rect ref = (cst.position == PositionType::Fixed) ? viewport : final_cb;

        float avail_pos_w = ref.w;
        if (cst.width > 0.0f) {
            avail_pos_w = cst.width + cst.margin.left + cst.margin.right;
        } else if (cst.has_left && cst.has_right) {
            avail_pos_w = std::max(60.0f, ref.w - cst.left - cst.right);
        } else if (ch->node && !ch->node->text_content().empty()) {
            float est = FontEngine::instance().measure_text(
                            ch->node->text_content(), cst.font_size, cst.bold, cst.monospace) +
                        cst.padding.left + cst.padding.right + cst.border_width * 2.0f + 16.0f;
            avail_pos_w = std::clamp(est, 60.0f, ref.w);
        }

        layout_node(ch, ref.x, ref.y, avail_pos_w, ref, viewport);

        float target_x = ref.x;
        float target_y = ref.y;
        if (cst.has_left) {
            target_x = ref.x + cst.left;
        } else if (cst.has_right) {
            target_x = ref.x + ref.w - ch->border_box.w - cst.right;
        }
        if (cst.has_top) {
            target_y = ref.y + cst.top;
        } else if (cst.has_bottom) {
            target_y = ref.y + ref.h - ch->border_box.h - cst.bottom;
        }

        offset_subtree(ch, target_x - ch->border_box.x, target_y - ch->border_box.y);
    }

    // Apply position: relative visual offset without disrupting sibling flow
    if (st.position == PositionType::Relative) {
        float dx = st.has_left ? st.left : (st.has_right ? -st.right : 0.0f);
        float dy = st.has_top ? st.top : (st.has_bottom ? -st.bottom : 0.0f);
        offset_subtree(frame, dx, dy);
    }
    // CSS Transforms Level 1 §3: `transform` NO participa en el layout; es una
    // operación de pintado. La traslación se aplica en el compositor a través
    // de la matriz afín del DisplayItem, no moviendo la caja de borde aquí.
    // (Los campos translate_x/translate_y quedan a 0 y sin efecto en el flujo.)
}

// Multiplica dos matrices afines 2x3 en el orden [a c e; b d f]: out = m * n.
void mul_affine(const float m[6], const float n[6], float out[6]) {
    out[0] = m[0] * n[0] + m[2] * n[1];
    out[1] = m[1] * n[0] + m[3] * n[1];
    out[2] = m[0] * n[2] + m[2] * n[3];
    out[3] = m[1] * n[2] + m[3] * n[3];
    out[4] = m[0] * n[4] + m[2] * n[5] + m[4];
    out[5] = m[1] * n[4] + m[3] * n[5] + m[5];
}

bool affine_is_identity(const float m[6]) {
    return m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f &&
           m[3] == 1.0f && m[4] == 0.0f && m[5] == 0.0f;
}

void emit_display_items(const LayoutFrame* f, DisplayList& dl,
                        int32_t inherited_z, bool inherited_fixed, float inherited_opacity,
                        bool has_clip, const Rect& clip_rect,
                        const float inherited_xform[6]) {
    if (!f) return;
    const ComputedStyle& st = f->style;

    int32_t eff_z = (st.position != PositionType::Static || st.z_index != 0)
                        ? std::max(inherited_z, static_cast<int32_t>(st.z_index))
                        : inherited_z;
    if (st.z_index != 0) eff_z = static_cast<int32_t>(st.z_index);
    bool eff_fixed = inherited_fixed || (st.position == PositionType::Fixed);
    float eff_opacity = inherited_opacity * st.opacity;
    if (eff_opacity <= 0.001f) return;

    // Session 8: `transform` se acumula por el subárbol, igual que en CSS: la
    // matriz del elemento se compone con la de sus ancestros transformados y se
    // calcula alrededor del centro de su propia caja de borde
    // (`transform-origin: 50% 50%`, el valor por defecto).
    float eff_xform[6];
    std::memcpy(eff_xform, inherited_xform, sizeof(eff_xform));
    if (st.transform.enabled && !st.transform.is_identity()) {
        float local[6];
        st.transform.to_matrix(f->border_box.x + f->border_box.w * 0.5f,
                               f->border_box.y + f->border_box.h * 0.5f,
                               local);
        float composed[6];
        mul_affine(inherited_xform, local, composed);
        std::memcpy(eff_xform, composed, sizeof(eff_xform));
    }
    const bool eff_has_xform = !affine_is_identity(eff_xform);

    auto attach_clip = [&](DisplayItem& it) {
        if (has_clip) {
            it.has_clip = true;
            it.clip_rect = clip_rect;
        }
        if (eff_has_xform) {
            it.has_transform = true;
            std::memcpy(it.xform, eff_xform, sizeof(it.xform));
        }
    };

    if (st.box_shadow.enabled && f->border_box.w > 0 && f->border_box.h > 0) {
        DisplayItem sh;
        sh.type = DisplayItemType::DropShadow;
        sh.rect = Rect{
            f->border_box.x + st.box_shadow.offset_x,
            f->border_box.y + st.box_shadow.offset_y,
            f->border_box.w,
            f->border_box.h
        };
        sh.color_argb = apply_opacity(st.box_shadow.color.to_argb(), eff_opacity);
        sh.radius = st.border_radius;
        sh.thickness = st.box_shadow.blur;
        sh.z_index = eff_z;
        sh.fixed_viewport = eff_fixed;
        attach_clip(sh);
        dl.items.push_back(std::move(sh));
    }

    // Un elemento con `mask-image` no pinta su fondo como bloque: emite un
    // MaskedFill, que el compositor tiñe a través del alfa de la máscara.
    const bool suppress_bg = st.has_unapplied_mask() || !st.background_image.empty();
    if (suppress_bg && st.background.a > 0 && f->border_box.w > 0 && f->border_box.h > 0) {
        DisplayItem item;
        item.type = DisplayItemType::MaskedFill;
        item.rect = f->border_box;
        item.color_argb = apply_opacity(st.background.to_argb(), eff_opacity);
        item.radius = st.border_radius;
        item.z_index = eff_z;
        item.fixed_viewport = eff_fixed;
        item.text = st.mask_image;
        attach_clip(item);
        dl.items.push_back(std::move(item));
    }

    if (!suppress_bg && st.background_gradient.enabled && f->border_box.w > 0 && f->border_box.h > 0) {
        DisplayItem item;
        item.type = DisplayItemType::FillGradient;
        item.rect = f->border_box;
        item.color_argb = apply_opacity(st.background_gradient.start.to_argb(), eff_opacity);
        item.color2_argb = apply_opacity(st.background_gradient.end.to_argb(), eff_opacity);
        item.angle_deg = st.background_gradient.angle_deg;
        item.radius = st.border_radius;
        item.z_index = eff_z;
        item.fixed_viewport = eff_fixed;
        attach_clip(item);
        dl.items.push_back(std::move(item));
    } else if (!suppress_bg && st.background.a > 0 && f->border_box.w > 0 && f->border_box.h > 0) {
        DisplayItem item;
        item.type = DisplayItemType::FillRect;
        item.rect = f->border_box;
        item.color_argb = apply_opacity(st.background.to_argb(), eff_opacity);
        item.radius = st.border_radius;
        item.z_index = eff_z;
        item.fixed_viewport = eff_fixed;
        attach_clip(item);
        dl.items.push_back(std::move(item));
    }

    // Imagen de fondo: se dibuja sobre el color, escalada a la caja. Cubre los
    // iconos que las interfaces declaran con `background-image` en vez de
    // `mask-image`, que sin esto quedaban como un rectángulo de color.
    if (!st.background_image.empty() && f->border_box.w > 0 && f->border_box.h > 0) {
        DisplayItem item;
        item.type = DisplayItemType::DrawImage;
        item.rect = f->border_box;
        item.z_index = eff_z;
        item.fixed_viewport = eff_fixed;
        item.text = st.background_image;
        item.radius = st.border_radius;
        attach_clip(item);
        dl.items.push_back(std::move(item));
    }

    if (st.border_width > 0.0f && st.border_color.a > 0) {
        DisplayItem item;
        item.type = DisplayItemType::StrokeRect;
        item.rect = f->border_box;
        item.color_argb = apply_opacity(st.border_color.to_argb(), eff_opacity);
        item.radius = st.border_radius;
        item.thickness = st.border_width;
        item.z_index = eff_z;
        item.fixed_viewport = eff_fixed;
        attach_clip(item);
        dl.items.push_back(std::move(item));
    }
    if (st.border_left_width > 0.0f && st.border_left_color.a > 0 && f->border_box.h > 0) {
        DisplayItem bl;
        bl.type = DisplayItemType::FillRect;
        bl.rect = Rect{f->border_box.x, f->border_box.y, st.border_left_width, f->border_box.h};
        bl.color_argb = apply_opacity(st.border_left_color.to_argb(), eff_opacity);
        bl.radius = std::min(st.border_radius, st.border_left_width * 0.5f);
        bl.z_index = eff_z;
        bl.fixed_viewport = eff_fixed;
        attach_clip(bl);
        dl.items.push_back(std::move(bl));
    }

    bool child_has_clip = has_clip;
    Rect child_clip = clip_rect;
    if (st.overflow == OverflowMode::Hidden) {
        child_clip = has_clip ? intersect_rect(clip_rect, f->border_box) : f->border_box;
        child_has_clip = true;
    }

    if (f->node && f->node->type == NodeType::Element) {
        const std::string& tag = f->node->tag;
        if (tag == "a") {
            std::string href = f->node->get_attr("href");
            if (!href.empty()) {
                bool is_dl = false;
                for (const auto& a : f->node->attrs) {
                    if (a.name == "download") { is_dl = true; break; }
                }
                dl.hit_regions.push_back(HitRegion{
                    is_dl ? HitRegionKind::Download : HitRegionKind::Link,
                    f->border_box, eff_z, eff_fixed, href
                });
            }
        } else if (tag == "button") {
            std::string id = f->node->id();
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Button, f->border_box, eff_z, eff_fixed, id.empty() ? "button" : ("#" + id)});
        } else if (tag == "astra-3d") {
            DisplayItem item;
            item.type = DisplayItemType::Embed3D;
            item.rect = f->content_box;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            item.text = f->node->get_attr("scene");
            if (item.text.empty()) item.text = f->node->get_attr("src");
            if (item.text.empty()) item.text = "planet";
            attach_clip(item);
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Scene3D, f->content_box, eff_z, eff_fixed, item.text});
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "video") {
            DisplayItem item;
            item.type = DisplayItemType::EmbedVideo;
            item.rect = f->content_box;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            item.text = resolve_media_src(f->node);
            attach_clip(item);
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Video, f->content_box, eff_z, eff_fixed, item.text});
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "audio") {
            DisplayItem item;
            item.type = DisplayItemType::EmbedAudio;
            item.rect = f->content_box;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            item.text = resolve_media_src(f->node);
            attach_clip(item);
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Audio, f->content_box, eff_z, eff_fixed, item.text});
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "canvas") {
            DisplayItem item;
            item.type = DisplayItemType::EmbedCanvas;
            item.rect = f->content_box;
            item.radius = st.border_radius;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            std::string cid = f->node->id();
            item.text = cid.empty() ? "canvas" : cid;
            attach_clip(item);
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "svg") {
            DisplayItem item;
            item.type = DisplayItemType::EmbedSvg;
            item.rect = f->content_box;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            std::string xml = "<svg";
            for (const auto& a : f->node->attrs) {
                xml += " " + a.name + "=\"" + a.value + "\"";
            }
            xml += ">" + f->node->inner_html() + "</svg>";
            item.text = std::move(xml);
            attach_clip(item);
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "img") {
            DisplayItem item;
            item.type = DisplayItemType::DrawImage;
            item.rect = f->content_box;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            item.text = resolve_img_src(f->node);
            attach_clip(item);
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "input" || tag == "textarea") {
            std::string id = f->node->id();
            std::string sel = id.empty() ? tag : ("#" + id);
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Input, f->border_box, eff_z, eff_fixed, sel});

            DisplayItem item;
            item.type = DisplayItemType::DrawInput;
            item.rect = f->border_box;
            item.radius = st.border_radius;
            item.font_size = st.font_size;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            item.focused = (f->node->get_attr("data-astra-focused") == "1");
            item.color2_argb = apply_opacity(st.background.to_argb(), eff_opacity);
            std::string val = f->node->get_attr("value");
            std::string ph = f->node->get_attr("placeholder");
            std::string input_type = f->node->get_attr("type");
            if (input_type == "checkbox" || input_type == "radio") {
                item.text = f->node->has_attr("checked") ? "x" : "";
                item.color_argb = apply_opacity(st.color.to_argb(), eff_opacity);
            } else if (!val.empty()) {
                if (input_type == "password") item.text.assign(val.size(), '*');
                else item.text = val;
                item.color_argb = apply_opacity(st.color.to_argb(), eff_opacity);
            } else {
                item.text = ph;
                item.color_argb = apply_opacity(0xFF64748Bu, eff_opacity);
            }
            attach_clip(item);
            dl.items.push_back(std::move(item));
            return;
        }
    }

    if (f->node && f->node->type == NodeType::Text && !f->wrapped_lines.empty()) {
        float lh = line_height_for_size(st.font_size);
        float ty = f->content_box.y;
        auto& fe = FontEngine::instance();
        for (const auto& line : f->wrapped_lines) {
            float tx = f->content_box.x;
            if (st.text_align == TextAlign::Center || st.text_align == TextAlign::Right) {
                float lw = fe.measure_text(line, st.font_size, st.bold, st.monospace);
                float spare = std::max(0.0f, f->content_box.w - lw);
                tx += (st.text_align == TextAlign::Center) ? (spare * 0.5f) : spare;
            }
            DisplayItem item;
            item.type = DisplayItemType::DrawText;
            item.rect = Rect{tx, ty, f->content_box.w, lh};
            item.color_argb = apply_opacity(st.color.to_argb(), eff_opacity);
            item.font_size = st.font_size;
            item.z_index = eff_z;
            item.bold = st.bold;
            item.monospace = st.monospace;
            item.fixed_viewport = eff_fixed;
            item.text = line;
            if (child_has_clip) {
                item.has_clip = true;
                item.clip_rect = child_clip;
            }
            dl.items.push_back(std::move(item));
            ty += lh;
        }
    }

    for (const LayoutFrame* ch : f->children) {
        emit_display_items(ch, dl, eff_z, eff_fixed, eff_opacity, child_has_clip, child_clip, eff_xform);
    }
}

} // namespace

const HitRegion* DisplayList::hit_test(float x, float y, float scroll_y) const {
    for (auto it = hit_regions.rbegin(); it != hit_regions.rend(); ++it) {
        float eff_y = it->fixed_viewport ? y : (y + scroll_y);
        const Rect& r = it->rect;
        if (x >= r.x && x <= r.x + r.w && eff_y >= r.y && eff_y <= r.y + r.h) {
            return &(*it);
        }
    }
    return nullptr;
}

std::vector<uint8_t> DisplayList::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(64 + items.size() * 76 + hit_regions.size() * 40);
    write_u32(buf, 0x41534436u); // Magic 'ASD6' (Session 8: matriz afín 2D por item)
    write_u32(buf, viewport_width);
    write_u32(buf, viewport_height);
    write_f32(buf, content_height);
    write_u32(buf, clear_color_argb);
    write_u32(buf, static_cast<uint32_t>(items.size()));
    for (const auto& it : items) {
        write_u32(buf, static_cast<uint32_t>(it.type));
        write_f32(buf, it.rect.x);
        write_f32(buf, it.rect.y);
        write_f32(buf, it.rect.w);
        write_f32(buf, it.rect.h);
        write_u32(buf, it.color_argb);
        write_u32(buf, it.color2_argb);
        write_f32(buf, it.angle_deg);
        write_f32(buf, it.radius);
        write_f32(buf, it.thickness);
        write_f32(buf, it.font_size);
        write_u32(buf, static_cast<uint32_t>(it.z_index));
        uint32_t flags = (it.bold ? 1u : 0u) | (it.monospace ? 2u : 0u) |
                         (it.focused ? 4u : 0u) | (it.fixed_viewport ? 8u : 0u) |
                         (it.has_clip ? 16u : 0u) | (it.has_transform ? 32u : 0u);
        write_u32(buf, flags);
        if (it.has_clip) {
            write_f32(buf, it.clip_rect.x);
            write_f32(buf, it.clip_rect.y);
            write_f32(buf, it.clip_rect.w);
            write_f32(buf, it.clip_rect.h);
        }
        if (it.has_transform) {
            for (int k = 0; k < 6; ++k) write_f32(buf, it.xform[k]);
        }
        write_str(buf, it.text);
    }
    write_u32(buf, static_cast<uint32_t>(hit_regions.size()));
    for (const auto& hr : hit_regions) {
        write_u32(buf, static_cast<uint32_t>(hr.kind));
        write_f32(buf, hr.rect.x);
        write_f32(buf, hr.rect.y);
        write_f32(buf, hr.rect.w);
        write_f32(buf, hr.rect.h);
        write_u32(buf, static_cast<uint32_t>(hr.z_index));
        write_u32(buf, hr.fixed_viewport ? 1u : 0u);
        write_str(buf, hr.target);
    }
    return buf;
}

bool DisplayList::deserialize(const std::vector<uint8_t>& blob, DisplayList& out) {
    size_t off = 0;
    uint32_t magic = 0, count = 0;
    if (!read_u32(blob, off, magic) ||
        (magic != 0x41534436u && magic != 0x41534435u &&
         magic != 0x41534434u && magic != 0x41534433u)) return false;
    bool is_v6 = (magic == 0x41534436u);
    bool is_v5 = is_v6 || (magic == 0x41534435u);
    if (!read_u32(blob, off, out.viewport_width)) return false;
    if (!read_u32(blob, off, out.viewport_height)) return false;
    if (!read_f32(blob, off, out.content_height)) return false;
    if (!read_u32(blob, off, out.clear_color_argb)) return false;
    if (!read_u32(blob, off, count)) return false;

    out.items.clear();
    out.items.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        DisplayItem it;
        uint32_t t = 0, z = 0, flags = 0;
        if (!read_u32(blob, off, t)) return false;
        it.type = static_cast<DisplayItemType>(t);
        if (!read_f32(blob, off, it.rect.x)) return false;
        if (!read_f32(blob, off, it.rect.y)) return false;
        if (!read_f32(blob, off, it.rect.w)) return false;
        if (!read_f32(blob, off, it.rect.h)) return false;
        if (!read_u32(blob, off, it.color_argb)) return false;
        if (is_v5) {
            if (!read_u32(blob, off, it.color2_argb)) return false;
            if (!read_f32(blob, off, it.angle_deg)) return false;
        }
        if (!read_f32(blob, off, it.radius)) return false;
        if (!read_f32(blob, off, it.thickness)) return false;
        if (!read_f32(blob, off, it.font_size)) return false;
        if (!read_u32(blob, off, z)) return false;
        it.z_index = static_cast<int32_t>(z);
        if (!read_u32(blob, off, flags)) return false;
        it.bold = (flags & 1u) != 0;
        it.monospace = (flags & 2u) != 0;
        it.focused = (flags & 4u) != 0;
        it.fixed_viewport = (flags & 8u) != 0;
        it.has_clip = (flags & 16u) != 0;
        if (it.has_clip) {
            if (!read_f32(blob, off, it.clip_rect.x)) return false;
            if (!read_f32(blob, off, it.clip_rect.y)) return false;
            if (!read_f32(blob, off, it.clip_rect.w)) return false;
            if (!read_f32(blob, off, it.clip_rect.h)) return false;
        }
        it.has_transform = is_v6 && ((flags & 32u) != 0);
        if (it.has_transform) {
            for (int k = 0; k < 6; ++k) {
                if (!read_f32(blob, off, it.xform[k])) return false;
            }
        }
        if (!read_str(blob, off, it.text)) return false;
        out.items.push_back(std::move(it));
    }

    uint32_t hr_count = 0;
    if (!read_u32(blob, off, hr_count)) return false;
    out.hit_regions.clear();
    out.hit_regions.reserve(hr_count);
    for (uint32_t i = 0; i < hr_count; ++i) {
        HitRegion hr;
        uint32_t k = 0, z = 0, fx = 0;
        if (!read_u32(blob, off, k)) return false;
        hr.kind = static_cast<HitRegionKind>(k);
        if (!read_f32(blob, off, hr.rect.x)) return false;
        if (!read_f32(blob, off, hr.rect.y)) return false;
        if (!read_f32(blob, off, hr.rect.w)) return false;
        if (!read_f32(blob, off, hr.rect.h)) return false;
        if (!read_u32(blob, off, z)) return false;
        hr.z_index = static_cast<int32_t>(z);
        if (!read_u32(blob, off, fx)) return false;
        hr.fixed_viewport = (fx != 0);
        if (!read_str(blob, off, hr.target)) return false;
        out.hit_regions.push_back(std::move(hr));
    }
    return true;
}

LayoutFrame* LayoutEngine::build_layout(SlabArena& arena,
                                        const HtmlDocument& doc,
                                        const CssStylesheet& sheet,
                                        float viewport_w,
                                        float viewport_h,
                                        bool light_web_default,
                                        AnimationTimeline* timeline) {
    const DomNode* body = doc.body();
    ComputedStyle root_seed;
    root_seed.css_vars = StyleEngine::collect_root_vars(sheet, viewport_w);
    const ComputedStyle* seed_ptr = root_seed.css_vars.empty() ? nullptr : &root_seed;
    if (timeline) timeline->attach(&sheet);
    LayoutFrame* root = build_frame_tree(arena, body, sheet, seed_ptr, viewport_w,
                                         light_web_default, timeline);
    if (!root) return nullptr;
    Rect viewport{0.0f, 0.0f, viewport_w, viewport_h};
    layout_node(root, 0.0f, 0.0f, viewport_w, viewport, viewport);
    if (root->border_box.h < viewport_h) {
        root->border_box.h = viewport_h;
    }
    return root;
}

DisplayList LayoutEngine::build_display_list(const LayoutFrame* root,
                                             uint32_t viewport_w,
                                             uint32_t viewport_h) {
    DisplayList dl;
    dl.viewport_width = viewport_w;
    dl.viewport_height = viewport_h;
    if (root) {
        if (root->style.background.a > 0) {
            dl.clear_color_argb = root->style.background.to_argb();
        }
        dl.content_height = root->border_box.h;
        static const float kIdentity[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        emit_display_items(root, dl, 0, false, 1.0f, false, Rect{}, kIdentity);
        // Stable-sort paint layers by z-index (preserving DOM order within equal z-index)
        std::stable_sort(dl.items.begin(), dl.items.end(),
                         [](const DisplayItem& a, const DisplayItem& b) {
                             return a.z_index < b.z_index;
                         });
        std::stable_sort(dl.hit_regions.begin(), dl.hit_regions.end(),
                         [](const HitRegion& a, const HitRegion& b) {
                             return a.z_index < b.z_index;
                         });
    }
    return dl;
}

} // namespace astra
