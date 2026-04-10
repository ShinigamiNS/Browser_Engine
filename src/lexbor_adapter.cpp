// lexbor_adapter.cpp
// Converts Lexbor parse tree to our Node tree.
// Uses direct struct field access to avoid API version mismatches.

#include "lexbor_adapter.h"
#include "dom.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/attr.h>

#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <cctype>

static std::string lxb_to_str(const lxb_char_t *data, size_t len) {
    if (!data || len == 0) return "";
    return std::string(reinterpret_cast<const char*>(data), len);
}

static std::string normalize_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool sp = false;
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!sp) { out += ' '; sp = true; }
        } else { out += (char)c; sp = false; }
    }
    return out;
}

static bool is_preserve_tag(const std::string& t) {
    return t=="pre"||t=="textarea"||t=="script"||t=="style";
}

// ── Get element local name ─────────────────────────────────────────────────
static std::string get_tag_name(lxb_dom_element_t *el) {
    size_t len = 0;
    // Try the qualified name first, fall back to tag_id
    const lxb_char_t *name = lxb_dom_element_local_name(el, &len);
    if (name && len > 0) {
        std::string tag = lxb_to_str(name, len);
        std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
        return tag;
    }
    // Fallback: use tag id (2-arg version)
    lxb_tag_id_t tid = lxb_dom_element_tag_id(el);
    const lxb_char_t *tname = lxb_tag_name_by_id(tid, &len);
    if (tname && len > 0) {
        std::string tag = lxb_to_str(tname, len);
        std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
        return tag;
    }
    return "unknown";
}

// ── Get attribute name ─────────────────────────────────────────────────────
static std::string get_attr_name(lxb_dom_attr_t *attr) {
    size_t len = 0;
    const lxb_char_t *name = lxb_dom_attr_local_name(attr, &len);
    if (name && len > 0) return lxb_to_str(name, len);
    return "";
}

// ── Get attribute value ────────────────────────────────────────────────────
static std::string get_attr_value(lxb_dom_attr_t *attr) {
    size_t len = 0;
    const lxb_char_t *val = lxb_dom_attr_value(attr, &len);
    if (val && len > 0) return lxb_to_str(val, len);
    return "";
}

// ── Iterate attributes using struct field directly ─────────────────────────
static lxb_dom_attr_t* attr_next(lxb_dom_attr_t *attr) {
    if (!attr) return nullptr;
    return attr->next;  // lxb_dom_attr_t has its own next/prev pointers
}

// ── Get first attribute ────────────────────────────────────────────────────
static lxb_dom_attr_t* first_attr(lxb_dom_element_t *el) {
    if (!el) return nullptr;
    return el->first_attr;
}

// ── Recursive node converter ───────────────────────────────────────────────
static std::shared_ptr<Node> convert_node(
    lxb_dom_node_t *lxnode,
    std::shared_ptr<Node> parent,
    bool preserve_ws)
{
    if (!lxnode) return nullptr;

    // Text node
    if (lxnode->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(lxnode);
        if (!cd || cd->data.length == 0) return nullptr;
        std::string text = lxb_to_str(cd->data.data, cd->data.length);
        if (!preserve_ws) text = normalize_ws(text);
        if (text.empty()) return nullptr;
        auto n = TextNode(text);
        n->parent = parent;
        return n;
    }

    // Element node
    if (lxnode->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t *el = lxb_dom_interface_element(lxnode);
        std::string tag = get_tag_name(el);

        auto node = ElementNode(tag);
        node->parent = parent;

        // Attributes
        lxb_dom_attr_t *attr = first_attr(el);
        while (attr) {
            std::string name = get_attr_name(attr);
            std::string val  = get_attr_value(attr);
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (!name.empty()) node->attributes[name] = val;
            // Advance: try API, fall back to struct
#ifdef LXB_DOM_ATTR_NEXT
            attr = lxb_dom_attr_next(attr);
#else
            attr = attr_next(attr);
#endif
        }

        // Children
        bool child_preserve = preserve_ws || is_preserve_tag(tag);
        std::shared_ptr<Node> prev;
        lxb_dom_node_t *child = lxb_dom_node_first_child(lxnode);
        while (child) {
            auto cn = convert_node(child, node, child_preserve);
            if (cn) {
                cn->parent = node;
                if (prev) { cn->prev_sibling = prev; prev->next_sibling = cn; }
                node->children.push_back(cn);
                prev = cn;
            }
            child = lxb_dom_node_next(child);
        }
        return node;
    }

    // Document — find and return <html> child
    if (lxnode->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
        lxb_dom_node_t *child = lxb_dom_node_first_child(lxnode);
        while (child) {
            if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element_t *el = lxb_dom_interface_element(child);
                std::string tag = get_tag_name(el);
                if (tag == "html")
                    return convert_node(child, parent, false);
            }
            child = lxb_dom_node_next(child);
        }
    }

    return nullptr; // comment, doctype, etc.
}

// ── Public API ─────────────────────────────────────────────────────────────
std::shared_ptr<Node> lexbor_parse_to_dom(const std::string& html) {
    if (html.empty()) return ElementNode("html");

    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return ElementNode("html");

    lxb_status_t st = lxb_html_document_parse(
        doc,
        reinterpret_cast<const lxb_char_t*>(html.c_str()),
        html.size());

    std::shared_ptr<Node> result;

    if (st == LXB_STATUS_OK) {
        // lxb_html_document_t is a superset of lxb_dom_document_t
        lxb_dom_node_t *doc_node =
            lxb_dom_interface_node(lxb_dom_interface_document(doc));

        // Find <html> element by walking document children
        result = convert_node(doc_node, nullptr, false);
    }

    lxb_html_document_destroy(doc);
    if (!result) return ElementNode("html");
    return result;
}

// ── CSS extraction ──────────────────────────────────────────────────────────
std::string lexbor_extract_css(const std::shared_ptr<Node>& root) {
    std::string css;
    if (!root) return css;
    std::function<void(const std::shared_ptr<Node>&)> walk;
    walk = [&](const std::shared_ptr<Node>& n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->data == "style") {
            for (auto& c : n->children)
                if (c && c->type == NodeType::Text)
                    css += "\n" + c->data + "\n";
            return;
        }
        for (auto& c : n->children) walk(c);
    };
    walk(root);
    return css;
}
