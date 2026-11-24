#include "layout/layout_engine.hpp"
#include "gfx/font_engine.hpp"
#include "ipc/ipc_channel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

std::vector<std::string> wrap_text(const std::string& raw, float max_width, float font_size, bool bold, bool mono) {
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

LayoutFrame* build_frame_tree(SlabArena& arena,
                              const DomNode* node,
                              const CssStylesheet& sheet,
                              const ComputedStyle* parent_style,
                              float viewport_w) {
    if (!node) return nullptr;

    if (node->type == NodeType::Text) {
        LayoutFrame* f = arena.create<LayoutFrame>();
        f->node = node;
        if (parent_style) f->style = *parent_style;
        f->style.display = DisplayType::Inline;
        f->style.position = PositionType::Static;
        f->style.overflow = OverflowMode::Visible;
        f->style.background = Color{0, 0, 0, 0};
        f->style.border_width = 0;
        f->style.padding = {};
        f->style.margin = {};
        f->style.box_shadow.enabled = false;
        f->style.opacity = 1.0f;
        return f;
    }

    ComputedStyle st = StyleEngine::compute_style(node, sheet, parent_style, viewport_w);
    if (st.display == DisplayType::None) return nullptr;

    // Default styling for interactive <button> if author didn't override
    if (node->tag == "button" && st.background.a == 0) {
        st.background = Color{2, 132, 199, 255};
        st.color = Color{255, 255, 255, 255};
        st.padding = {8, 16, 8, 16};
        st.border_radius = 6.0f;
        st.bold = true;
    }

    LayoutFrame* f = arena.create<LayoutFrame>();
    f->node = node;
    f->style = st;

    for (const DomNode* ch : node->children) {
        if (LayoutFrame* cf = build_frame_tree(arena, ch, sheet, &f->style, viewport_w)) {
            f->children.push_back(cf);
        }
    }
    return f;
}

void layout_node(LayoutFrame* frame, float x, float y, float avail_w,
                 const Rect& containing_block, const Rect& viewport) {
    if (!frame) return;
    const ComputedStyle& st = frame->style;

    float box_x = x + st.margin.left;
    float box_y = y + st.margin.top;
    float pad_x = st.padding.left + st.border_width;
    float pad_y = st.padding.top + st.border_width;
    float horiz_extra = pad_x + st.padding.right + st.border_width;
    float vert_extra = pad_y + st.padding.bottom + st.border_width;
    float max_avail = std::max(40.0f, avail_w - st.margin.left - st.margin.right);

    float box_w = max_avail;
    if (st.width > 0.0f) {
        float target_border_w = (st.box_sizing == BoxSizing::BorderBox)
                                    ? st.width
                                    : (st.width + horiz_extra);
        box_w = std::min(target_border_w, max_avail);
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
            float w = (st.width > 0.0f) ? st.width : (aw > 0.0f ? aw : std::min(inner_w, 280.0f));
            float h = (st.height > 0.0f) ? st.height : (ah > 0.0f ? ah : 160.0f);
            frame->content_box.w = std::min(w, inner_w);
            frame->content_box.h = h;
            frame->border_box.w = frame->content_box.w + horiz_extra;
            frame->border_box.h = h + vert_extra;
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
        frame->wrapped_lines = wrap_text(frame->node->text, inner_w, st.font_size, st.bold, st.monospace);
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

    if (st.display == DisplayType::Grid && !flow_children.empty()) {
        std::vector<GridTrack> cols = st.grid_columns;
        if (cols.empty()) cols = {GridTrack{1.0f, true}, GridTrack{1.0f, true}};
        size_t ncols = cols.size();
        float total_gap = st.gap * static_cast<float>(ncols > 1 ? ncols - 1 : 0);
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
            cx += col_w[c] + st.gap;
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
            if (idx < flow_children.size()) cur_y += st.gap;
        }
    } else if (st.display == DisplayType::Flex && st.flex_direction == FlexDirection::Row && !flow_children.empty()) {
        size_t count = flow_children.size();
        float total_gap = st.gap * static_cast<float>(count > 0 ? count - 1 : 0);
        float item_w = std::max(40.0f, (inner_w - total_gap) / static_cast<float>(count));
        float cur_x = frame->content_box.x;
        float max_h = 0.0f;
        for (LayoutFrame* ch : flow_children) {
            layout_node(ch, cur_x, cur_y, item_w, next_cb, viewport);
            float ch_total_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
            max_h = std::max(max_h, ch_total_h);
            cur_x += item_w + st.gap;
        }
        cur_y += max_h;
    } else {
        bool first = true;
        for (LayoutFrame* ch : flow_children) {
            if (!first && st.display == DisplayType::Flex) cur_y += st.gap;
            first = false;
            layout_node(ch, frame->content_box.x, cur_y, inner_w, next_cb, viewport);
            cur_y = ch->border_box.y + ch->border_box.h + ch->style.margin.bottom;
        }
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
}

void emit_display_items(const LayoutFrame* f, DisplayList& dl,
                        int32_t inherited_z, bool inherited_fixed, float inherited_opacity,
                        bool has_clip, const Rect& clip_rect) {
    if (!f) return;
    const ComputedStyle& st = f->style;

    int32_t eff_z = (st.position != PositionType::Static || st.z_index != 0)
                        ? std::max(inherited_z, static_cast<int32_t>(st.z_index))
                        : inherited_z;
    if (st.z_index != 0) eff_z = static_cast<int32_t>(st.z_index);
    bool eff_fixed = inherited_fixed || (st.position == PositionType::Fixed);
    float eff_opacity = inherited_opacity * st.opacity;
    if (eff_opacity <= 0.001f) return;

    auto attach_clip = [&](DisplayItem& it) {
        if (has_clip) {
            it.has_clip = true;
            it.clip_rect = clip_rect;
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

    if (st.background_gradient.enabled && f->border_box.w > 0 && f->border_box.h > 0) {
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
    } else if (st.background.a > 0 && f->border_box.w > 0 && f->border_box.h > 0) {
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
            item.text = f->node->get_attr("src");
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
            item.text = f->node->get_attr("src");
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
        } else if (tag == "img") {
            DisplayItem item;
            item.type = DisplayItemType::DrawImage;
            item.rect = f->content_box;
            item.z_index = eff_z;
            item.fixed_viewport = eff_fixed;
            item.text = f->node->get_attr("src");
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
            std::string val = f->node->get_attr("value");
            std::string ph = f->node->get_attr("placeholder");
            std::string input_type = f->node->get_attr("type");
            if (!val.empty()) {
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
        for (const auto& line : f->wrapped_lines) {
            DisplayItem item;
            item.type = DisplayItemType::DrawText;
            item.rect = Rect{f->content_box.x, ty, f->content_box.w, lh};
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
        emit_display_items(ch, dl, eff_z, eff_fixed, eff_opacity, child_has_clip, child_clip);
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
    write_u32(buf, 0x41534435u); // Magic 'ASD5' (Session 6: FillGradient + EmbedCanvas + WebRender clip_rect)
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
                         (it.has_clip ? 16u : 0u);
        write_u32(buf, flags);
        if (it.has_clip) {
            write_f32(buf, it.clip_rect.x);
            write_f32(buf, it.clip_rect.y);
            write_f32(buf, it.clip_rect.w);
            write_f32(buf, it.clip_rect.h);
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
        (magic != 0x41534435u && magic != 0x41534434u && magic != 0x41534433u)) return false;
    bool is_v5 = (magic == 0x41534435u);
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
                                        float viewport_h) {
    const DomNode* body = doc.body();
    LayoutFrame* root = build_frame_tree(arena, body, sheet, nullptr, viewport_w);
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
        emit_display_items(root, dl, 0, false, 1.0f, false, Rect{});
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
