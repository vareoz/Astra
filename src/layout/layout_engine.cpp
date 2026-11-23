#include "layout/layout_engine.hpp"
#include "ipc/ipc_channel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace astra {

namespace {

int glyph_scale_for_size(float font_size) {
    if (font_size >= 26.0f) return 3;
    if (font_size >= 18.0f) return 2;
    return 1;
}

float char_advance(float font_size, bool bold) {
    int scale = glyph_scale_for_size(font_size);
    return static_cast<float>(scale * 8 + (bold ? 1 : 0));
}

float line_height_for_size(float font_size) {
    int scale = glyph_scale_for_size(font_size);
    return static_cast<float>(scale * 8) * 1.65f;
}

std::vector<std::string> wrap_text(const std::string& raw, float max_width, float font_size, bool bold) {
    std::vector<std::string> lines;
    float adv = char_advance(font_size, bold);
    size_t max_chars = std::max<size_t>(8, static_cast<size_t>(max_width / adv));

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
        } else if (cur.size() + 1 + word.size() <= max_chars) {
            cur.push_back(' ');
            cur += word;
        } else {
            lines.push_back(std::move(cur));
            cur = std::move(word);
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

LayoutFrame* build_frame_tree(SlabArena& arena,
                              const DomNode* node,
                              const CssStylesheet& sheet,
                              const ComputedStyle* parent_style) {
    if (!node) return nullptr;

    if (node->type == NodeType::Text) {
        LayoutFrame* f = arena.create<LayoutFrame>();
        f->node = node;
        if (parent_style) f->style = *parent_style;
        f->style.display = DisplayType::Inline;
        f->style.background = Color{0, 0, 0, 0};
        f->style.border_width = 0;
        f->style.padding = {};
        f->style.margin = {};
        return f;
    }

    ComputedStyle st = StyleEngine::compute_style(node, sheet, parent_style);
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
        if (LayoutFrame* cf = build_frame_tree(arena, ch, sheet, &f->style)) {
            f->children.push_back(cf);
        }
    }
    return f;
}

void layout_node(LayoutFrame* frame, float x, float y, float avail_w) {
    if (!frame) return;
    const ComputedStyle& st = frame->style;

    float box_x = x + st.margin.left;
    float box_y = y + st.margin.top;
    float box_w = (st.width > 0.0f)
                      ? std::min(st.width, avail_w - st.margin.left - st.margin.right)
                      : std::max(40.0f, avail_w - st.margin.left - st.margin.right);

    float pad_x = st.padding.left + st.border_width;
    float pad_y = st.padding.top + st.border_width;
    float inner_w = std::max(24.0f, box_w - pad_x - st.padding.right - st.border_width);

    frame->border_box.x = box_x;
    frame->border_box.y = box_y;
    frame->border_box.w = box_w;

    frame->content_box.x = box_x + pad_x;
    frame->content_box.y = box_y + pad_y;
    frame->content_box.w = inner_w;

    // Replaced / embedded elements (<astra-3d>, <video>, <img>)
    if (frame->node && frame->node->type == NodeType::Element) {
        const std::string& tag = frame->node->tag;
        if (tag == "astra-3d" || tag == "video") {
            float h = (st.height > 0.0f) ? st.height : 260.0f;
            frame->content_box.h = h;
            frame->border_box.h = h + pad_y + st.padding.bottom + st.border_width;
            return;
        }
        if (tag == "img") {
            float aw = static_cast<float>(std::atoi(frame->node->get_attr("width").c_str()));
            float ah = static_cast<float>(std::atoi(frame->node->get_attr("height").c_str()));
            float w = (st.width > 0.0f) ? st.width : (aw > 0.0f ? aw : std::min(inner_w, 280.0f));
            float h = (st.height > 0.0f) ? st.height : (ah > 0.0f ? ah : 160.0f);
            frame->content_box.w = std::min(w, inner_w);
            frame->content_box.h = h;
            frame->border_box.w = frame->content_box.w + pad_x + st.padding.right + st.border_width;
            frame->border_box.h = h + pad_y + st.padding.bottom + st.border_width;
            return;
        }
    }

    // Text frame
    if (frame->node && frame->node->type == NodeType::Text) {
        frame->wrapped_lines = wrap_text(frame->node->text, inner_w, st.font_size, st.bold);
        float lh = line_height_for_size(st.font_size);
        frame->content_box.h = static_cast<float>(frame->wrapped_lines.size()) * lh;
        frame->border_box.h = frame->content_box.h;
        return;
    }

    float cur_y = frame->content_box.y;

    if (st.display == DisplayType::Flex && st.flex_direction == FlexDirection::Row && !frame->children.empty()) {
        size_t count = frame->children.size();
        float total_gap = st.gap * static_cast<float>(count > 0 ? count - 1 : 0);
        float item_w = std::max(40.0f, (inner_w - total_gap) / static_cast<float>(count));
        float cur_x = frame->content_box.x;
        float max_h = 0.0f;
        for (LayoutFrame* ch : frame->children) {
            layout_node(ch, cur_x, cur_y, item_w);
            float ch_total_h = ch->border_box.h + ch->style.margin.top + ch->style.margin.bottom;
            max_h = std::max(max_h, ch_total_h);
            cur_x += item_w + st.gap;
        }
        cur_y += max_h;
    } else {
        bool first = true;
        for (LayoutFrame* ch : frame->children) {
            if (!first && st.display == DisplayType::Flex) cur_y += st.gap;
            first = false;
            layout_node(ch, frame->content_box.x, cur_y, inner_w);
            cur_y = ch->border_box.y + ch->border_box.h + ch->style.margin.bottom;
        }
    }

    float measured_h = std::max(0.0f, cur_y - frame->content_box.y);
    if (st.height > 0.0f) measured_h = std::max(measured_h, st.height);
    frame->content_box.h = measured_h;
    frame->border_box.h = measured_h + pad_y + st.padding.bottom + st.border_width;
}

void emit_display_items(const LayoutFrame* f, DisplayList& dl) {
    if (!f) return;
    const ComputedStyle& st = f->style;

    if (st.background.a > 0 && f->border_box.w > 0 && f->border_box.h > 0) {
        DisplayItem item;
        item.type = DisplayItemType::FillRect;
        item.rect = f->border_box;
        item.color_argb = st.background.to_argb();
        item.radius = st.border_radius;
        dl.items.push_back(std::move(item));
    }

    if (st.border_width > 0.0f && st.border_color.a > 0) {
        DisplayItem item;
        item.type = DisplayItemType::StrokeRect;
        item.rect = f->border_box;
        item.color_argb = st.border_color.to_argb();
        item.radius = st.border_radius;
        item.thickness = st.border_width;
        dl.items.push_back(std::move(item));
    }

    if (f->node && f->node->type == NodeType::Element) {
        const std::string& tag = f->node->tag;
        if (tag == "a") {
            std::string href = f->node->get_attr("href");
            if (!href.empty()) {
                dl.hit_regions.push_back(HitRegion{HitRegionKind::Link, f->border_box, href});
            }
        } else if (tag == "button") {
            std::string id = f->node->id();
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Button, f->border_box, id.empty() ? "button" : ("#" + id)});
        } else if (tag == "astra-3d") {
            DisplayItem item;
            item.type = DisplayItemType::Embed3D;
            item.rect = f->content_box;
            item.text = f->node->get_attr("scene");
            if (item.text.empty()) item.text = f->node->get_attr("src");
            if (item.text.empty()) item.text = "planet";
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Scene3D, f->content_box, item.text});
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "video") {
            DisplayItem item;
            item.type = DisplayItemType::EmbedVideo;
            item.rect = f->content_box;
            item.text = f->node->get_attr("src");
            dl.hit_regions.push_back(HitRegion{HitRegionKind::Video, f->content_box, item.text});
            dl.items.push_back(std::move(item));
            return;
        } else if (tag == "img") {
            DisplayItem item;
            item.type = DisplayItemType::DrawImage;
            item.rect = f->content_box;
            item.text = f->node->get_attr("src");
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
            item.color_argb = st.color.to_argb();
            item.font_size = st.font_size;
            item.bold = st.bold;
            item.text = line;
            dl.items.push_back(std::move(item));
            ty += lh;
        }
    }

    for (const LayoutFrame* ch : f->children) {
        emit_display_items(ch, dl);
    }
}

} // namespace

const HitRegion* DisplayList::hit_test(float x, float y, float scroll_y) const {
    float doc_y = y + scroll_y;
    for (auto it = hit_regions.rbegin(); it != hit_regions.rend(); ++it) {
        const Rect& r = it->rect;
        if (x >= r.x && x <= r.x + r.w && doc_y >= r.y && doc_y <= r.y + r.h) {
            return &(*it);
        }
    }
    return nullptr;
}

std::vector<uint8_t> DisplayList::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(64 + items.size() * 48 + hit_regions.size() * 32);
    write_u32(buf, 0x41534432u); // Magic 'ASD2' (Session 2 format with HitRegions)
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
        write_f32(buf, it.radius);
        write_f32(buf, it.thickness);
        write_f32(buf, it.font_size);
        write_u32(buf, it.bold ? 1u : 0u);
        write_str(buf, it.text);
    }
    write_u32(buf, static_cast<uint32_t>(hit_regions.size()));
    for (const auto& hr : hit_regions) {
        write_u32(buf, static_cast<uint32_t>(hr.kind));
        write_f32(buf, hr.rect.x);
        write_f32(buf, hr.rect.y);
        write_f32(buf, hr.rect.w);
        write_f32(buf, hr.rect.h);
        write_str(buf, hr.target);
    }
    return buf;
}

bool DisplayList::deserialize(const std::vector<uint8_t>& blob, DisplayList& out) {
    size_t off = 0;
    uint32_t magic = 0, count = 0;
    if (!read_u32(blob, off, magic) || magic != 0x41534432u) return false;
    if (!read_u32(blob, off, out.viewport_width)) return false;
    if (!read_u32(blob, off, out.viewport_height)) return false;
    if (!read_f32(blob, off, out.content_height)) return false;
    if (!read_u32(blob, off, out.clear_color_argb)) return false;
    if (!read_u32(blob, off, count)) return false;

    out.items.clear();
    out.items.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        DisplayItem it;
        uint32_t t = 0, bold_flag = 0;
        if (!read_u32(blob, off, t)) return false;
        it.type = static_cast<DisplayItemType>(t);
        if (!read_f32(blob, off, it.rect.x)) return false;
        if (!read_f32(blob, off, it.rect.y)) return false;
        if (!read_f32(blob, off, it.rect.w)) return false;
        if (!read_f32(blob, off, it.rect.h)) return false;
        if (!read_u32(blob, off, it.color_argb)) return false;
        if (!read_f32(blob, off, it.radius)) return false;
        if (!read_f32(blob, off, it.thickness)) return false;
        if (!read_f32(blob, off, it.font_size)) return false;
        if (!read_u32(blob, off, bold_flag)) return false;
        it.bold = (bold_flag != 0);
        if (!read_str(blob, off, it.text)) return false;
        out.items.push_back(std::move(it));
    }

    uint32_t hr_count = 0;
    if (!read_u32(blob, off, hr_count)) return false;
    out.hit_regions.clear();
    out.hit_regions.reserve(hr_count);
    for (uint32_t i = 0; i < hr_count; ++i) {
        HitRegion hr;
        uint32_t k = 0;
        if (!read_u32(blob, off, k)) return false;
        hr.kind = static_cast<HitRegionKind>(k);
        if (!read_f32(blob, off, hr.rect.x)) return false;
        if (!read_f32(blob, off, hr.rect.y)) return false;
        if (!read_f32(blob, off, hr.rect.w)) return false;
        if (!read_f32(blob, off, hr.rect.h)) return false;
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
    LayoutFrame* root = build_frame_tree(arena, body, sheet, nullptr);
    if (!root) return nullptr;
    layout_node(root, 0.0f, 0.0f, viewport_w);
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
        emit_display_items(root, dl);
    }
    return dl;
}

} // namespace astra
