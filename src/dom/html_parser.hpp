#pragma once

#include "core/arena.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

enum class NodeType : uint8_t {
    Document,
    Element,
    Text
};

struct DomAttr {
    std::string name;
    std::string value;
};

struct DomNode {
    NodeType type = NodeType::Element;
    std::string tag;       // lowercase tag name for Element
    std::string text;      // text content for Text
    std::vector<DomAttr> attrs;
    DomNode* parent = nullptr;
    std::vector<DomNode*> children;

    std::string get_attr(std::string_view name) const;
    bool has_attr(std::string_view name) const;
    void set_attr(std::string_view name, std::string_view value);
    void remove_attr(std::string_view name);
    bool has_class(std::string_view cls) const;
    void add_class(std::string_view cls);
    void remove_class(std::string_view cls);
    bool toggle_class(std::string_view cls);
    std::string id() const { return get_attr("id"); }
    std::string text_content() const;
    void set_text_content(SlabArena& arena, std::string_view new_text);
    std::string inner_html() const;
    void set_inner_html(SlabArena& arena, std::string_view html);
};

struct ScriptEntry {
    std::string src;
    std::string inline_code;
    bool is_module = false;
    bool defer = false;
};

class HtmlDocument {
public:
    explicit HtmlDocument(SlabArena& arena);

    DomNode* root() const { return root_; }
    DomNode* body() const;
    std::string title() const;
    std::string collect_stylesheets() const;
    std::vector<std::string> collect_scripts() const;
    std::vector<ScriptEntry> collect_script_entries() const;

    DomNode* query_selector(std::string_view selector) const;
    std::vector<DomNode*> query_selector_all(std::string_view selector) const;

    size_t node_count() const { return node_count_; }
    SlabArena& arena() { return arena_; }

    static HtmlDocument parse(SlabArena& arena, std::string_view html);

private:
    SlabArena& arena_;
    DomNode* root_ = nullptr;
    size_t node_count_ = 0;
};

} // namespace astra
