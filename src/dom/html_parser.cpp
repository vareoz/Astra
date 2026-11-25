#include "dom/html_parser.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

namespace astra {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_void_tag(std::string_view tag) {
    static const std::unordered_set<std::string_view> kVoid = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    return kVoid.count(tag) > 0;
}

std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i + 1);
            if (semi != std::string_view::npos && semi - i <= 10) {
                std::string_view ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out.push_back('&'); i = semi; continue; }
                if (ent == "lt") { out.push_back('<'); i = semi; continue; }
                if (ent == "gt") { out.push_back('>'); i = semi; continue; }
                if (ent == "quot") { out.push_back('"'); i = semi; continue; }
                if (ent == "apos" || ent == "#39") { out.push_back('\''); i = semi; continue; }
                if (ent == "nbsp") { out.push_back(' '); i = semi; continue; }
                if (ent == "copy") { out += "(c)"; i = semi; continue; }
                if (ent == "mdash") { out += "--"; i = semi; continue; }
                if (ent == "ndash") { out += "-"; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    unsigned code = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        code = std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16);
                    } else {
                        code = std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
                    }
                    if (code >= 32 && code < 127) {
                        out.push_back(static_cast<char>(code));
                        i = semi;
                        continue;
                    }
                }
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool matches_simple_selector(const DomNode* node, std::string_view sel) {
    if (!node || node->type != NodeType::Element || sel.empty()) return false;
    while (!sel.empty() && std::isspace(static_cast<unsigned char>(sel.front()))) sel.remove_prefix(1);
    while (!sel.empty() && std::isspace(static_cast<unsigned char>(sel.back()))) sel.remove_suffix(1);
    if (sel == "*") return true;
    if (sel[0] == '#') return node->id() == sel.substr(1);
    if (sel[0] == '.') return node->has_class(sel.substr(1));

    // Support tag.class or tag#id
    size_t dot = sel.find('.');
    if (dot != std::string_view::npos) {
        return node->tag == to_lower(sel.substr(0, dot)) && node->has_class(sel.substr(dot + 1));
    }
    size_t hash = sel.find('#');
    if (hash != std::string_view::npos) {
        return node->tag == to_lower(sel.substr(0, hash)) && node->id() == sel.substr(hash + 1);
    }
    return node->tag == to_lower(sel);
}

void collect_matches(DomNode* node, std::string_view sel, std::vector<DomNode*>& out, bool first_only) {
    if (!node) return;
    if (first_only && !out.empty()) return;
    if (node->type == NodeType::Element && matches_simple_selector(node, sel)) {
        out.push_back(node);
        if (first_only) return;
    }
    for (DomNode* ch : node->children) {
        collect_matches(ch, sel, out, first_only);
        if (first_only && !out.empty()) return;
    }
}

void collect_tags_text(const DomNode* node, std::string_view tag, std::vector<std::string>& out) {
    if (!node) return;
    if (node->type == NodeType::Element && node->tag == tag) {
        out.push_back(node->text_content());
        return;
    }
    for (const DomNode* ch : node->children) {
        collect_tags_text(ch, tag, out);
    }
}

} // namespace

std::string DomNode::get_attr(std::string_view name) const {
    std::string low = to_lower(name);
    for (const auto& a : attrs) {
        if (a.name == low) return a.value;
    }
    return {};
}

bool DomNode::has_attr(std::string_view name) const {
    std::string low = to_lower(name);
    for (const auto& a : attrs) {
        if (a.name == low) return true;
    }
    return false;
}

void DomNode::set_attr(std::string_view name, std::string_view value) {
    std::string low = to_lower(name);
    for (auto& a : attrs) {
        if (a.name == low) {
            a.value = std::string(value);
            return;
        }
    }
    attrs.push_back(DomAttr{std::move(low), std::string(value)});
}

void DomNode::remove_attr(std::string_view name) {
    std::string low = to_lower(name);
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                               [&](const DomAttr& a) { return a.name == low; }),
                attrs.end());
}

bool DomNode::has_class(std::string_view cls) const {
    std::string classes = get_attr("class");
    if (classes.empty()) return false;
    size_t pos = 0;
    while (pos < classes.size()) {
        while (pos < classes.size() && std::isspace(static_cast<unsigned char>(classes[pos]))) ++pos;
        size_t start = pos;
        while (pos < classes.size() && !std::isspace(static_cast<unsigned char>(classes[pos]))) ++pos;
        if (pos > start && std::string_view(classes.data() + start, pos - start) == cls) {
            return true;
        }
    }
    return false;
}

void DomNode::add_class(std::string_view cls) {
    if (cls.empty() || has_class(cls)) return;
    std::string cur = get_attr("class");
    if (!cur.empty()) cur.push_back(' ');
    cur.append(cls);
    set_attr("class", cur);
}

void DomNode::remove_class(std::string_view cls) {
    std::string cur = get_attr("class");
    if (cur.empty()) return;
    std::string rebuilt;
    size_t pos = 0;
    while (pos < cur.size()) {
        while (pos < cur.size() && std::isspace(static_cast<unsigned char>(cur[pos]))) ++pos;
        size_t start = pos;
        while (pos < cur.size() && !std::isspace(static_cast<unsigned char>(cur[pos]))) ++pos;
        if (pos > start) {
            std::string_view tok(cur.data() + start, pos - start);
            if (tok != cls) {
                if (!rebuilt.empty()) rebuilt.push_back(' ');
                rebuilt.append(tok);
            }
        }
    }
    set_attr("class", rebuilt);
}

bool DomNode::toggle_class(std::string_view cls) {
    if (has_class(cls)) {
        remove_class(cls);
        return false;
    }
    add_class(cls);
    return true;
}

std::string DomNode::text_content() const {
    if (type == NodeType::Text) return text;
    std::string out;
    for (const DomNode* ch : children) {
        out += ch->text_content();
    }
    return out;
}

void DomNode::set_text_content(SlabArena& arena, std::string_view new_text) {
    children.clear();
    DomNode* t = arena.create<DomNode>();
    t->type = NodeType::Text;
    t->text = std::string(new_text);
    t->parent = this;
    children.push_back(t);
}

namespace {
void serialize_node_html(const DomNode* n, std::string& out) {
    if (!n) return;
    if (n->type == NodeType::Text) {
        out += n->text;
        return;
    }
    if (n->type == NodeType::Element) {
        out.push_back('<');
        out += n->tag;
        for (const auto& a : n->attrs) {
            out.push_back(' ');
            out += a.name;
            out += "=\"";
            out += a.value;
            out.push_back('"');
        }
        out.push_back('>');
        if (!is_void_tag(n->tag)) {
            for (const DomNode* ch : n->children) serialize_node_html(ch, out);
            out += "</";
            out += n->tag;
            out.push_back('>');
        }
    }
}
} // namespace

std::string DomNode::inner_html() const {
    std::string out;
    for (const DomNode* ch : children) serialize_node_html(ch, out);
    return out;
}

void DomNode::set_inner_html(SlabArena& arena, std::string_view html) {
    children.clear();
    if (html.empty()) return;
    HtmlDocument frag = HtmlDocument::parse(arena, html);
    DomNode* src_root = frag.root();
    if (!src_root) return;
    for (DomNode* ch : src_root->children) {
        ch->parent = this;
        children.push_back(ch);
    }
}

HtmlDocument::HtmlDocument(SlabArena& arena) : arena_(arena) {
    root_ = arena_.create<DomNode>();
    root_->type = NodeType::Document;
    root_->tag = "#document";
    node_count_ = 1;
}

DomNode* HtmlDocument::body() const {
    DomNode* b = query_selector("body");
    return b ? b : root_;
}

std::string HtmlDocument::title() const {
    DomNode* t = query_selector("title");
    if (!t) return {};
    std::string s = t->text_content();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string HtmlDocument::collect_stylesheets() const {
    std::vector<std::string> sheets;
    collect_tags_text(root_, "style", sheets);
    std::string joined;
    for (const auto& s : sheets) {
        joined += s;
        joined.push_back('\n');
    }
    return joined;
}

std::vector<std::string> HtmlDocument::collect_scripts() const {
    std::vector<std::string> scripts;
    collect_tags_text(root_, "script", scripts);
    return scripts;
}

std::vector<ScriptEntry> HtmlDocument::collect_script_entries() const {
    std::vector<ScriptEntry> out;
    std::function<void(const DomNode*)> walk = [&](const DomNode* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tag == "script") {
            std::string type_attr = to_lower(n->get_attr("type"));
            if (!type_attr.empty() && type_attr != "text/javascript" &&
                type_attr != "application/javascript" && type_attr != "module") {
                return; // Skip JSON-LD, importmap, template data scripts
            }
            ScriptEntry se;
            se.src = n->get_attr("src");
            se.inline_code = n->text_content();
            se.is_module = (type_attr == "module");
            se.defer = n->has_attr("defer");
            if (!se.src.empty() || !se.inline_code.empty()) {
                out.push_back(std::move(se));
            }
            return;
        }
        for (const DomNode* ch : n->children) walk(ch);
    };
    walk(root_);
    return out;
}

DomNode* HtmlDocument::query_selector(std::string_view selector) const {
    std::vector<DomNode*> matches;
    collect_matches(root_, selector, matches, true);
    return matches.empty() ? nullptr : matches[0];
}

std::vector<DomNode*> HtmlDocument::query_selector_all(std::string_view selector) const {
    std::vector<DomNode*> matches;
    collect_matches(root_, selector, matches, false);
    return matches;
}

HtmlDocument HtmlDocument::parse(SlabArena& arena, std::string_view html) {
    HtmlDocument doc(arena);
    std::vector<DomNode*> stack;
    stack.push_back(doc.root_);

    size_t pos = 0;
    const size_t n = html.size();

    while (pos < n) {
        if (html[pos] == '<') {
            // Comment
            if (html.substr(pos, 4) == "<!--") {
                size_t end = html.find("-->", pos + 4);
                pos = (end == std::string_view::npos) ? n : end + 3;
                continue;
            }
            // DOCTYPE or processing instruction
            if (pos + 1 < n && (html[pos + 1] == '!' || html[pos + 1] == '?')) {
                size_t end = html.find('>', pos + 2);
                pos = (end == std::string_view::npos) ? n : end + 1;
                continue;
            }
            // Closing tag
            if (pos + 1 < n && html[pos + 1] == '/') {
                size_t name_start = pos + 2;
                size_t name_end = name_start;
                while (name_end < n && !std::isspace(static_cast<unsigned char>(html[name_end])) && html[name_end] != '>') {
                    ++name_end;
                }
                std::string close_tag = to_lower(html.substr(name_start, name_end - name_start));
                size_t gt = html.find('>', name_end);
                pos = (gt == std::string_view::npos) ? n : gt + 1;

                // Pop up to matching tag
                for (size_t i = stack.size(); i-- > 1;) {
                    if (stack[i]->tag == close_tag) {
                        stack.resize(i);
                        break;
                    }
                }
                continue;
            }

            // Opening tag
            size_t name_start = pos + 1;
            size_t name_end = name_start;
            while (name_end < n && !std::isspace(static_cast<unsigned char>(html[name_end])) &&
                   html[name_end] != '>' && html[name_end] != '/') {
                ++name_end;
            }
            if (name_end == name_start) {
                ++pos;
                continue;
            }
            std::string tag = to_lower(html.substr(name_start, name_end - name_start));

            DomNode* elem = arena.create<DomNode>();
            elem->type = NodeType::Element;
            elem->tag = tag;
            elem->parent = stack.back();
            stack.back()->children.push_back(elem);
            ++doc.node_count_;

            // Parse attributes
            pos = name_end;
            bool self_closing = false;
            while (pos < n) {
                while (pos < n && std::isspace(static_cast<unsigned char>(html[pos]))) ++pos;
                if (pos >= n) break;
                if (html[pos] == '>') {
                    ++pos;
                    break;
                }
                if (html[pos] == '/' && pos + 1 < n && html[pos + 1] == '>') {
                    self_closing = true;
                    pos += 2;
                    break;
                }
                size_t a_start = pos;
                while (pos < n && !std::isspace(static_cast<unsigned char>(html[pos])) &&
                       html[pos] != '=' && html[pos] != '>' && html[pos] != '/') {
                    ++pos;
                }
                if (pos == a_start) { ++pos; continue; }
                std::string attr_name = to_lower(html.substr(a_start, pos - a_start));
                while (pos < n && std::isspace(static_cast<unsigned char>(html[pos]))) ++pos;
                std::string attr_val;
                if (pos < n && html[pos] == '=') {
                    ++pos;
                    while (pos < n && std::isspace(static_cast<unsigned char>(html[pos]))) ++pos;
                    if (pos < n && (html[pos] == '"' || html[pos] == '\'')) {
                        char quote = html[pos++];
                        size_t v_start = pos;
                        while (pos < n && html[pos] != quote) ++pos;
                        attr_val = decode_entities(html.substr(v_start, pos - v_start));
                        if (pos < n && html[pos] == quote) ++pos;
                    } else {
                        size_t v_start = pos;
                        while (pos < n && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '>') ++pos;
                        attr_val = decode_entities(html.substr(v_start, pos - v_start));
                    }
                }
                elem->attrs.push_back(DomAttr{std::move(attr_name), std::move(attr_val)});
            }

            // Raw text elements (<style>, <script>)
            if (!self_closing && (tag == "style" || tag == "script")) {
                std::string close_seq = "</" + tag;
                size_t end_raw = pos;
                while (end_raw < n) {
                    size_t lt = html.find('<', end_raw);
                    if (lt == std::string_view::npos) { end_raw = n; break; }
                    if (lt + close_seq.size() <= n &&
                        to_lower(html.substr(lt, close_seq.size())) == close_seq) {
                        end_raw = lt;
                        break;
                    }
                    end_raw = lt + 1;
                }
                if (end_raw > pos) {
                    DomNode* txt = arena.create<DomNode>();
                    txt->type = NodeType::Text;
                    txt->text = std::string(html.substr(pos, end_raw - pos));
                    txt->parent = elem;
                    elem->children.push_back(txt);
                    ++doc.node_count_;
                }
                size_t close_gt = html.find('>', end_raw);
                pos = (close_gt == std::string_view::npos) ? n : close_gt + 1;
                continue;
            }

            if (!self_closing && !is_void_tag(tag)) {
                stack.push_back(elem);
            }
        } else {
            size_t next_lt = html.find('<', pos);
            std::string_view raw = (next_lt == std::string_view::npos) ? html.substr(pos) : html.substr(pos, next_lt - pos);
            pos = (next_lt == std::string_view::npos) ? n : next_lt;

            std::string decoded = decode_entities(raw);
            bool all_ws = std::all_of(decoded.begin(), decoded.end(),
                                      [](unsigned char c) { return std::isspace(c); });
            if (!all_ws) {
                DomNode* txt = arena.create<DomNode>();
                txt->type = NodeType::Text;
                txt->text = std::move(decoded);
                txt->parent = stack.back();
                stack.back()->children.push_back(txt);
                ++doc.node_count_;
            }
        }
    }

    return doc;
}

} // namespace astra
