// quickjs_adapter.cpp
// Bridges QuickJS (full ES2020) to our Node DOM tree.
// Replaces the toy js.cpp engine entirely.

#include "quickjs_adapter.h"
#include "dom.h"

extern "C" {
#include "quickjs.h"
}

#include <string>
#include <memory>
#include <functional>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <windows.h>

// ── Per-eval execution deadline (used by interrupt handler) ────────────────
static thread_local std::chrono::steady_clock::time_point g_js_deadline;

static int js_interrupt_handler(JSRuntime * /*rt*/, void * /*opaque*/) {
    return std::chrono::steady_clock::now() > g_js_deadline ? 1 : 0;
}

// ── Engine state ───────────────────────────────────────────────────────────
struct QJSEngine {
    JSRuntime *rt  = nullptr;
    JSContext *ctx = nullptr;
    std::shared_ptr<Node> dom_root;
    bool dom_dirty = false;

    // Per-node event listeners: node* -> event_type -> [JSValue callbacks]
    std::map<Node*, std::map<std::string, std::vector<JSValue>>> event_listeners;

    // JS-created nodes kept alive here (document.createElement)
    std::vector<std::shared_ptr<Node>> created_nodes;

    // Viewport size (for window.innerWidth/innerHeight)
    int viewport_width  = 800;
    int viewport_height = 600;
    int scroll_y        = 0;

    // Current page URL (for window.location)
    std::string page_url;

    // Layout query callback (for getBoundingClientRect)
    std::function<bool(Node*, DOMRect&)> layout_cb;

    // localStorage in-memory store
    std::map<std::string, std::string> local_storage;

    // Pending setTimeout/setInterval callbacks (fired by qjs_run_pending_timers)
    struct PendingTimer {
        int id;
        JSValue callback;
        std::vector<JSValue> extra_args;
    };
    std::vector<PendingTimer> pending_timers;
    int next_timer_id = 1;
};

// ── JS value helpers ───────────────────────────────────────────────────────
static std::string js_to_string(JSContext *ctx, JSValue val) {
    size_t len;
    const char *str = JS_ToCStringLen(ctx, &len, val);
    if (!str) return "";
    std::string s(str, len);
    JS_FreeCString(ctx, str);
    return s;
}

static JSValue js_from_string(JSContext *ctx, const std::string& s) {
    return JS_NewStringLen(ctx, s.c_str(), s.size());
}

// ── Node → JSValue wrapping ────────────────────────────────────────────────
// We store a raw Node* as opaque pointer in a JS object.
// The Node is kept alive by the shared_ptr in the DOM tree.

static JSClassID node_class_id = 0;

static void node_finalizer(JSRuntime *rt, JSValue val) {
    // Node is owned by the DOM tree; we don't delete it here
}

static JSClassDef node_class_def = {
    "Node",
    .finalizer = node_finalizer,
};

static Node* get_node(JSContext *ctx, JSValue obj) {
    return static_cast<Node*>(JS_GetOpaque(obj, node_class_id));
}

static JSValue wrap_node(JSContext *ctx, std::shared_ptr<Node> node,
                         QJSEngine *engine);

// ── DOM method implementations ─────────────────────────────────────────────

// ── Inline-style helpers ───────────────────────────────────────────────────
// Convert a CSS property name written in camelCase to kebab-case.
// e.g. "backgroundColor" → "background-color"
static std::string camel_to_kebab(const std::string &s) {
    std::string r;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') { r += '-'; r += (char)(c - 'A' + 'a'); }
        else r += c;
    }
    return r;
}

// Read a single CSS property value from a node's inline style string.
static std::string inline_style_get(Node *node, const std::string &prop_kebab) {
    if (!node) return "";
    auto it = node->attributes.find("style");
    if (it == node->attributes.end()) return "";
    const std::string &s = it->second;
    // Search for "prop_kebab:" in the inline style
    size_t pos = 0;
    while (pos < s.size()) {
        size_t col = s.find(':', pos);
        if (col == std::string::npos) break;
        // Extract key (trimmed)
        size_t ks = s.find_first_not_of(" \t;", pos);
        size_t ke = s.find_last_not_of(" \t", col - 1);
        if (ks != std::string::npos && ke != std::string::npos && ke >= ks) {
            std::string key = s.substr(ks, ke - ks + 1);
            if (key == prop_kebab) {
                size_t vs = s.find_first_not_of(" \t", col + 1);
                if (vs == std::string::npos) return "";
                size_t semi = s.find(';', vs);
                std::string val = s.substr(vs, semi == std::string::npos ? semi : semi - vs);
                // trim trailing space
                size_t ve = val.find_last_not_of(" \t");
                return ve != std::string::npos ? val.substr(0, ve + 1) : val;
            }
        }
        size_t semi = s.find(';', col);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;
    }
    return "";
}

// Write a single CSS property into a node's inline style string.
// Removes any existing value for that property first.
static void inline_style_set(Node *node, const std::string &prop_kebab,
                              const std::string &val) {
    if (!node) return;
    // Allow all display changes — Google's init JS legitimately shows hidden elements.
    std::string &s = node->attributes["style"];
    // Remove existing declaration for this property
    size_t pos = 0;
    while (pos < s.size()) {
        size_t col = s.find(':', pos);
        if (col == std::string::npos) break;
        size_t ks = s.find_first_not_of(" \t;", pos);
        size_t ke = (ks != std::string::npos && ks < col)
                    ? s.find_last_not_of(" \t", col - 1) : std::string::npos;
        if (ks != std::string::npos && ke != std::string::npos && ke >= ks) {
            std::string key = s.substr(ks, ke - ks + 1);
            if (key == prop_kebab) {
                size_t semi = s.find(';', col);
                size_t end = (semi == std::string::npos) ? s.size() : semi + 1;
                s.erase(pos, end - pos);
                continue; // re-check from same pos
            }
        }
        size_t semi = s.find(';', col);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;
    }
    if (!val.empty()) {
        if (!s.empty() && s.back() != ';') s += ';';
        s += prop_kebab + ':' + val + ';';
    }
}

// Global __style_get(nodePtr, prop) — used by the JS Proxy factory
static JSValue js__style_get(JSContext *ctx, JSValue /*this_val*/,
                              int argc, JSValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    int64_t p = 0; JS_ToInt64(ctx, &p, argv[0]);
    Node *node = reinterpret_cast<Node*>((uintptr_t)p);
    std::string prop = js_to_string(ctx, argv[1]);
    if (prop == "_ptr" || prop == "setProperty" || prop == "removeProperty")
        return JS_UNDEFINED; // internal / function props
    std::string kebab = camel_to_kebab(prop);
    return js_from_string(ctx, inline_style_get(node, kebab));
}

// Global __style_set(nodePtr, enginePtr, prop, value) — used by JS Proxy factory
static JSValue js__style_set(JSContext *ctx, JSValue /*this_val*/,
                              int argc, JSValue *argv) {
    if (argc < 4) return JS_UNDEFINED;
    int64_t p = 0;  JS_ToInt64(ctx, &p, argv[0]);
    int64_t ep = 0; JS_ToInt64(ctx, &ep, argv[1]);
    Node *node    = reinterpret_cast<Node*>((uintptr_t)p);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ep);
    std::string prop = js_to_string(ctx, argv[2]);
    std::string val  = js_to_string(ctx, argv[3]);
    if (prop.empty() || prop[0] == '_') return JS_UNDEFINED;
    std::string kebab = camel_to_kebab(prop);
    inline_style_set(node, kebab, val);
    if (eng) eng->dom_dirty = true;
    return JS_UNDEFINED;
}

// setProperty(name, value[, priority]) — CSS OM method on style object
static JSValue js_style_set_property(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    JSValue np = JS_GetPropertyStr(ctx, this_val, "_ptr");
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t pi = 0, ei = 0;
    JS_ToInt64(ctx, &pi, np); JS_FreeValue(ctx, np);
    JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    Node *node = reinterpret_cast<Node*>((uintptr_t)pi);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    std::string prop = js_to_string(ctx, argv[0]);
    std::string val  = js_to_string(ctx, argv[1]);
    inline_style_set(node, prop, val);
    if (eng) eng->dom_dirty = true;
    return JS_UNDEFINED;
}

static JSValue js_style_remove_property(JSContext *ctx, JSValue this_val,
                                         int argc, JSValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    JSValue np = JS_GetPropertyStr(ctx, this_val, "_ptr");
    int64_t pi = 0; JS_ToInt64(ctx, &pi, np); JS_FreeValue(ctx, np);
    Node *node = reinterpret_cast<Node*>((uintptr_t)pi);
    std::string prop = js_to_string(ctx, argv[0]);
    inline_style_set(node, prop, ""); // empty value = remove
    return JS_UNDEFINED;
}

// Global __innerHTML_set(ptr, engPtr, value) — replaces element's children with a text node
static JSValue js__innerHTML_set(JSContext *ctx, JSValue /*this_val*/,
                                  int argc, JSValue *argv) {
    if (argc < 3) return JS_UNDEFINED;
    int64_t p = 0, ep = 0;
    JS_ToInt64(ctx, &p,  argv[0]);
    JS_ToInt64(ctx, &ep, argv[1]);
    Node *node = reinterpret_cast<Node*>((uintptr_t)p);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ep);
    std::string val = js_to_string(ctx, argv[2]);
    if (!node) return JS_UNDEFINED;
    // Replace children: if val has no tags, make a single text node
    node->children.clear();
    if (!val.empty()) {
        auto text_node = std::make_shared<Node>(NodeType::Text, val);
        text_node->parent = node->shared_from_this();
        node->children.push_back(text_node);
    }
    if (eng) eng->dom_dirty = true;
    return JS_UNDEFINED;
}

static JSValue js_set_attribute(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node *node = get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    std::string name = js_to_string(ctx, argv[0]);
    std::string val  = js_to_string(ctx, argv[1]);
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    node->attributes[name] = val;
    // Mark engine dirty via _eng pointer stored on element
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t ei = 0; JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    if (eng) eng->dom_dirty = true;
    return JS_UNDEFINED;
}

static JSValue js_get_attribute(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    if (argc < 1) return JS_NULL;
    Node *node = get_node(ctx, this_val);
    if (!node) return JS_NULL;
    std::string name = js_to_string(ctx, argv[0]);
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    auto it = node->attributes.find(name);
    if (it == node->attributes.end()) return JS_NULL;
    return js_from_string(ctx, it->second);
}

static JSValue js_remove_attribute(JSContext *ctx, JSValue this_val,
                                    int argc, JSValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    Node *node = get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    std::string name = js_to_string(ctx, argv[0]);
    node->attributes.erase(name);
    return JS_UNDEFINED;
}

// Helper: find a shared_ptr<Node> in the DOM tree by raw pointer
static std::shared_ptr<Node> find_shared(const std::shared_ptr<Node> &root, Node *raw) {
    if (!root) return nullptr;
    if (root.get() == raw) return root;
    for (auto &c : root->children) {
        auto r = find_shared(c, raw);
        if (r) return r;
    }
    return nullptr;
}

// Helper: also search engine's created_nodes for orphan JS-created nodes
static std::shared_ptr<Node> find_shared_any(QJSEngine *eng, Node *raw) {
    if (!eng) return nullptr;
    auto r = find_shared(eng->dom_root, raw);
    if (r) return r;
    for (auto &n : eng->created_nodes)
        if (n.get() == raw) return n;
    return nullptr;
}

static JSValue js_append_child(JSContext *ctx, JSValue this_val,
                                int argc, JSValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    Node *parent = get_node(ctx, this_val);
    Node *child  = get_node(ctx, argv[0]);
    if (!parent || !child) return JS_UNDEFINED;
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t ei = 0; JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    auto child_sp = find_shared_any(eng, child);
    if (!child_sp) return JS_UNDEFINED;
    // Remove from current parent if present
    auto &ch = child_sp->children; (void)ch;
    parent->children.push_back(child_sp);
    if (eng) eng->dom_dirty = true;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_insert_before(JSContext *ctx, JSValue this_val,
                                 int argc, JSValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    Node *parent = get_node(ctx, this_val);
    Node *child  = get_node(ctx, argv[0]);
    if (!parent || !child) return JS_UNDEFINED;
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t ei = 0; JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    auto child_sp = find_shared_any(eng, child);
    if (!child_sp) return JS_UNDEFINED;
    Node *ref = (argc >= 2) ? get_node(ctx, argv[1]) : nullptr;
    if (!ref) {
        parent->children.push_back(child_sp);
    } else {
        auto it = std::find_if(parent->children.begin(), parent->children.end(),
                               [ref](const std::shared_ptr<Node> &n) { return n.get() == ref; });
        parent->children.insert(it, child_sp);
    }
    if (eng) eng->dom_dirty = true;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_replace_child(JSContext *ctx, JSValue this_val,
                                  int argc, JSValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node *parent   = get_node(ctx, this_val);
    Node *new_child = get_node(ctx, argv[0]);
    Node *old_child = get_node(ctx, argv[1]);
    if (!parent || !new_child || !old_child) return JS_UNDEFINED;
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t ei = 0; JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    auto new_sp = find_shared_any(eng, new_child);
    if (!new_sp) return JS_UNDEFINED;
    for (auto &c : parent->children) {
        if (c.get() == old_child) { c = new_sp; break; }
    }
    if (eng) eng->dom_dirty = true;
    return JS_DupValue(ctx, argv[1]); // return old child
}

// addEventListener on a DOM element — stores callback in engine registry
static JSValue js_add_event_listener(JSContext *ctx, JSValue this_val,
                                      int argc, JSValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node *node = get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t ei = 0; JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    if (!eng) return JS_UNDEFINED;
    std::string type = js_to_string(ctx, argv[0]);
    if (JS_IsFunction(ctx, argv[1])) {
        eng->event_listeners[node][type].push_back(JS_DupValue(ctx, argv[1]));
    }
    return JS_UNDEFINED;
}

static JSValue js_remove_event_listener(JSContext *ctx, JSValue this_val,
                                          int argc, JSValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node *node = get_node(ctx, this_val);
    if (!node) return JS_UNDEFINED;
    JSValue ep = JS_GetPropertyStr(ctx, this_val, "_eng");
    int64_t ei = 0; JS_ToInt64(ctx, &ei, ep); JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
    if (!eng) return JS_UNDEFINED;
    std::string type = js_to_string(ctx, argv[0]);
    auto it = eng->event_listeners.find(node);
    if (it != eng->event_listeners.end()) {
        it->second.erase(type);
    }
    return JS_UNDEFINED;
}

static JSValue js_remove_child(JSContext *ctx, JSValue this_val,
                                int argc, JSValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    Node *parent = get_node(ctx, this_val);
    Node *child  = get_node(ctx, argv[0]);
    if (!parent || !child) return JS_UNDEFINED;
    parent->children.erase(
        std::remove_if(parent->children.begin(), parent->children.end(),
            [child](const std::shared_ptr<Node>& n) { return n.get() == child; }),
        parent->children.end());
    return JS_UNDEFINED;
}

// ── classList ──────────────────────────────────────────────────────────────
// NOTE: classList.add is intentionally a no-op.
// Google's init JS calls classList.add('fH2tV') and similar on ancestors
// which triggers CSS rules like .fH2tV .FPdoLc{visibility:hidden} that hide
// the search buttons. Since we render a static snapshot we don't want
// JS-driven state changes to alter the initial class list.
static JSValue make_classlist(JSContext *ctx, Node *node, QJSEngine *engine) {
    JSValue cl = JS_NewObject(ctx);

    // add(name) — no-op: prevents JS from adding state classes that break layout
    JS_SetPropertyStr(ctx, cl, "add",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue /*this_v*/,
                                int /*ac*/, JSValue* /*av*/) -> JSValue {
            return JS_UNDEFINED;
        }, "add", 1));

    // remove(name)
    JS_SetPropertyStr(ctx, cl, "remove",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue np = JS_GetPropertyStr(c, this_v, "_np");
            int64_t p = 0; JS_ToInt64(c, &p, np); JS_FreeValue(c, np);
            Node *nd = reinterpret_cast<Node*>((uintptr_t)p);
            if (!nd || ac < 1) return JS_UNDEFINED;
            std::string cls = " " + js_to_string(c, av[0]);
            std::string& cur = nd->attributes["class"];
            auto pos = cur.find(cls);
            if (pos != std::string::npos) cur.erase(pos, cls.size());
            else {
                cls = cls.substr(1); // try without leading space
                pos = cur.find(cls);
                if (pos != std::string::npos) cur.erase(pos, cls.size());
            }
            return JS_UNDEFINED;
        }, "remove", 1));

    // toggle(name)
    JS_SetPropertyStr(ctx, cl, "toggle",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue np = JS_GetPropertyStr(c, this_v, "_np");
            int64_t p = 0; JS_ToInt64(c, &p, np); JS_FreeValue(c, np);
            Node *nd = reinterpret_cast<Node*>((uintptr_t)p);
            if (!nd || ac < 1) return JS_FALSE;
            std::string cls = js_to_string(c, av[0]);
            std::string& cur = nd->attributes["class"];
            if (cur.find(cls) != std::string::npos) {
                // remove
                auto pos = cur.find(cls);
                cur.erase(pos, cls.size());
                return JS_FALSE;
            } else {
                if (!cur.empty()) cur += " ";
                cur += cls;
                return JS_TRUE;
            }
        }, "toggle", 1));

    // contains(name)
    JS_SetPropertyStr(ctx, cl, "contains",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue np = JS_GetPropertyStr(c, this_v, "_np");
            int64_t p = 0; JS_ToInt64(c, &p, np); JS_FreeValue(c, np);
            Node *nd = reinterpret_cast<Node*>((uintptr_t)p);
            if (!nd || ac < 1) return JS_FALSE;
            std::string cls = js_to_string(c, av[0]);
            std::string& cur = nd->attributes["class"];
            return cur.find(cls) != std::string::npos ? JS_TRUE : JS_FALSE;
        }, "contains", 1));

    JS_SetPropertyStr(ctx, cl, "_np",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)node));
    return cl;
}

// Collect all text from a subtree
static void collect_text(Node *n, std::string &out, int depth = 0) {
    if (!n || depth > 50 || out.size() > 100000) return;
    // Skip script/style content
    if (n->type == NodeType::Element &&
        (n->data == "script" || n->data == "style")) return;
    if (n->type == NodeType::Text) out += n->data;
    for (auto &c : n->children) collect_text(c.get(), out, depth + 1);
}

// ── Wrap a Node as a JS object ─────────────────────────────────────────────
static JSValue wrap_node(JSContext *ctx, std::shared_ptr<Node> node,
                          QJSEngine *engine) {
    if (!node) return JS_NULL;

    JSValue obj = JS_NewObjectClass(ctx, node_class_id);
    JS_SetOpaque(obj, node.get());

    Node *raw = node.get();

    // Store engine pointer on every element for method callbacks
    JS_SetPropertyStr(ctx, obj, "_eng",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));

    // tagName / nodeName
    std::string tag_upper = raw->type == NodeType::Element ? raw->data : "#text";
    for (char &c : tag_upper) c = (char)::toupper((unsigned char)c);
    JS_SetPropertyStr(ctx, obj, "tagName",   js_from_string(ctx, tag_upper));
    JS_SetPropertyStr(ctx, obj, "nodeName",  js_from_string(ctx, tag_upper));
    JS_SetPropertyStr(ctx, obj, "localName", js_from_string(ctx, raw->data));

    // nodeType: 1=Element, 3=Text
    JS_SetPropertyStr(ctx, obj, "nodeType",
        JS_NewInt32(ctx, raw->type == NodeType::Element ? 1 : 3));

    // id, className
    {
        auto it = raw->attributes.find("id");
        JS_SetPropertyStr(ctx, obj, "id",
            js_from_string(ctx, it != raw->attributes.end() ? it->second : ""));
    }
    {
        auto it = raw->attributes.find("class");
        JS_SetPropertyStr(ctx, obj, "className",
            js_from_string(ctx, it != raw->attributes.end() ? it->second : ""));
    }

    // textContent / innerText as static values (setter below)
    {
        std::string text;
        collect_text(raw, text);
        JS_SetPropertyStr(ctx, obj, "textContent", js_from_string(ctx, text));
        JS_SetPropertyStr(ctx, obj, "innerText",   js_from_string(ctx, text));
        JS_SetPropertyStr(ctx, obj, "nodeValue",
            raw->type == NodeType::Text ? js_from_string(ctx, raw->data) : JS_NULL);
    }

    // innerHTML — getter reads children text, setter modifies DOM
    {
        // Closure data: [0]=node ptr as int64, [1]=engine ptr as int64
        JSValue data[2];
        data[0] = JS_NewInt64(ctx, (int64_t)(uintptr_t)raw);
        data[1] = JS_NewInt64(ctx, (int64_t)(uintptr_t)engine);
        JSValue getter = JS_NewCFunctionData(ctx,
            [](JSContext *c, JSValue tv, int, JSValue *, int, JSValue *d) -> JSValue {
                int64_t p = 0; JS_ToInt64(c, &p, d[0]);
                Node *nd = reinterpret_cast<Node*>((uintptr_t)p);
                if (!nd) return JS_NewString(c, "");
                std::string html;
                for (auto &ch : nd->children) {
                    if (!ch) continue;
                    if (ch->type == NodeType::Text) html += ch->data;
                    else if (ch->type == NodeType::Element) {
                        html += "<" + ch->data + ">";
                        std::string t; collect_text(ch.get(), t); html += t;
                        html += "</" + ch->data + ">";
                    }
                }
                return JS_NewStringLen(c, html.c_str(), html.size());
            }, 0, 0, 2, data);
        JSValue setter = JS_NewCFunctionData(ctx,
            [](JSContext *c, JSValue tv, int argc, JSValue *argv, int, JSValue *d) -> JSValue {
                if (argc < 1) return JS_UNDEFINED;
                int64_t p = 0, ep = 0;
                JS_ToInt64(c, &p, d[0]);
                JS_ToInt64(c, &ep, d[1]);
                Node *nd = reinterpret_cast<Node*>((uintptr_t)p);
                QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ep);
                if (!nd) return JS_UNDEFINED;
                std::string val = js_to_string(c, argv[0]);
                nd->children.clear();
                if (!val.empty()) {
                    auto tn = std::make_shared<Node>(NodeType::Text, val);
                    // Set parent using find_shared_any (safe) instead of shared_from_this
                    if (eng) {
                        auto nd_sp = find_shared_any(eng, nd);
                        if (nd_sp) tn->parent = nd_sp;
                    }
                    nd->children.push_back(tn);
                }
                if (eng) eng->dom_dirty = true;
                return JS_UNDEFINED;
            }, 1, 0, 2, data);
        JSAtom atom = JS_NewAtom(ctx, "innerHTML");
        JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter,
            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    }

    // style — Proxy built in JS so direct assignment (el.style.display='none') works
    // __style_get / __style_set are registered as globals in setup_globals
    {
        std::string style_js =
            "__make_style_proxy(" +
            std::to_string((int64_t)(uintptr_t)raw) + "," +
            std::to_string((int64_t)(uintptr_t)engine) + ")";
        JSValue sv = JS_Eval(ctx, style_js.c_str(), style_js.size(),
                             "<style>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(sv)) {
            JS_FreeValue(ctx, sv);
            // Fallback: plain object with setProperty
            sv = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, sv, "_ptr",
                JS_NewInt64(ctx, (int64_t)(uintptr_t)raw));
            JS_SetPropertyStr(ctx, sv, "_eng",
                JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));
            JS_SetPropertyStr(ctx, sv, "setProperty",
                JS_NewCFunction(ctx, js_style_set_property, "setProperty", 2));
            JS_SetPropertyStr(ctx, sv, "removeProperty",
                JS_NewCFunction(ctx, js_style_remove_property, "removeProperty", 1));
        }
        JS_SetPropertyStr(ctx, obj, "style", sv);
    }

    // classList
    JS_SetPropertyStr(ctx, obj, "classList",
        make_classlist(ctx, raw, engine));

    // Methods
    JS_SetPropertyStr(ctx, obj, "setAttribute",
        JS_NewCFunction(ctx, js_set_attribute, "setAttribute", 2));
    JS_SetPropertyStr(ctx, obj, "getAttribute",
        JS_NewCFunction(ctx, js_get_attribute, "getAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "removeAttribute",
        JS_NewCFunction(ctx, js_remove_attribute, "removeAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "hasAttribute",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_FALSE;
            Node *nd = get_node(c, tv);
            if (!nd) return JS_FALSE;
            std::string n = js_to_string(c, av[0]);
            return nd->attributes.count(n) ? JS_TRUE : JS_FALSE;
        }, "hasAttribute", 1));
    JS_SetPropertyStr(ctx, obj, "appendChild",
        JS_NewCFunction(ctx, js_append_child, "appendChild", 1));
    JS_SetPropertyStr(ctx, obj, "insertBefore",
        JS_NewCFunction(ctx, js_insert_before, "insertBefore", 2));
    JS_SetPropertyStr(ctx, obj, "replaceChild",
        JS_NewCFunction(ctx, js_replace_child, "replaceChild", 2));
    JS_SetPropertyStr(ctx, obj, "removeChild",
        JS_NewCFunction(ctx, js_remove_child, "removeChild", 1));
    JS_SetPropertyStr(ctx, obj, "addEventListener",
        JS_NewCFunction(ctx, js_add_event_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, obj, "removeEventListener",
        JS_NewCFunction(ctx, js_remove_event_listener, "removeEventListener", 1));
    JS_SetPropertyStr(ctx, obj, "dispatchEvent",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_TRUE;
        }, "dispatchEvent", 1));

    // querySelector / querySelectorAll on element (use engine ptr)
    JS_SetPropertyStr(ctx, obj, "querySelector",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            Node *nd = get_node(c, tv);
            if (!eng || !nd || ac < 1) return JS_NULL;
            auto nd_sp = find_shared_any(eng, nd);
            if (!nd_sp) return JS_NULL;
            auto found = nd_sp->query_selector(js_to_string(c, av[0]));
            return found ? wrap_node(c, found, eng) : JS_NULL;
        }, "querySelector", 1));
    JS_SetPropertyStr(ctx, obj, "querySelectorAll",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            Node *nd = get_node(c, tv);
            if (!eng || !nd || ac < 1) return JS_NewArray(c);
            auto nd_sp = find_shared_any(eng, nd);
            if (!nd_sp) return JS_NewArray(c);
            auto results = nd_sp->query_selector_all(js_to_string(c, av[0]));
            JSValue arr = JS_NewArray(c);
            for (uint32_t i = 0; i < (uint32_t)results.size(); i++)
                JS_SetPropertyUint32(c, arr, i, wrap_node(c, results[i], eng));
            return arr;
        }, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, obj, "closest",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_NULL;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_NULL;
            Node *nd = static_cast<Node*>(p);
            std::string sel = js_to_string(c, av[0]);
            // Walk up from nd itself (including self)
            auto cur = find_shared_any(eng, nd);
            while (cur && cur->type == NodeType::Element) {
                // Check if cur matches by seeing if query_selector on cur returns cur itself
                auto test = cur->query_selector(sel);
                if (test && test.get() == cur.get())
                    return wrap_node(c, cur, eng);
                cur = cur->parent.lock();
            }
            return JS_NULL;
        }, "closest", 1));
    JS_SetPropertyStr(ctx, obj, "matches",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_FALSE;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_FALSE;
            Node *nd = static_cast<Node*>(p);
            std::string sel = js_to_string(c, av[0]);
            auto sp = find_shared_any(eng, nd);
            if (!sp) return JS_FALSE;
            auto test = sp->query_selector(sel);
            return JS_NewBool(c, test && test.get() == sp.get());
        }, "matches", 1));

    // getBoundingClientRect() — uses layout callback if available
    JS_SetPropertyStr(ctx, obj, "getBoundingClientRect",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int, JSValue*) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            Node *nd = get_node(c, tv);
            JSValue r = JS_NewObject(c);
            float x = 0, y = 0, w = 0, h = 0;
            if (eng && nd && eng->layout_cb) {
                DOMRect dr{};
                if (eng->layout_cb(nd, dr)) { x=dr.x; y=dr.y; w=dr.width; h=dr.height; }
            }
            JS_SetPropertyStr(c, r, "x",      JS_NewFloat64(c, x));
            JS_SetPropertyStr(c, r, "y",      JS_NewFloat64(c, y));
            JS_SetPropertyStr(c, r, "left",   JS_NewFloat64(c, x));
            JS_SetPropertyStr(c, r, "top",    JS_NewFloat64(c, y));
            JS_SetPropertyStr(c, r, "right",  JS_NewFloat64(c, x + w));
            JS_SetPropertyStr(c, r, "bottom", JS_NewFloat64(c, y + h));
            JS_SetPropertyStr(c, r, "width",  JS_NewFloat64(c, w));
            JS_SetPropertyStr(c, r, "height", JS_NewFloat64(c, h));
            return r;
        }, "getBoundingClientRect", 0));

    // offsetWidth / offsetHeight / clientWidth / clientHeight
    {
        DOMRect dr{};
        if (engine && engine->layout_cb) engine->layout_cb(raw, dr);
        JS_SetPropertyStr(ctx, obj, "offsetWidth",  JS_NewFloat64(ctx, dr.width));
        JS_SetPropertyStr(ctx, obj, "offsetHeight", JS_NewFloat64(ctx, dr.height));
        JS_SetPropertyStr(ctx, obj, "clientWidth",  JS_NewFloat64(ctx, dr.width));
        JS_SetPropertyStr(ctx, obj, "clientHeight", JS_NewFloat64(ctx, dr.height));
        JS_SetPropertyStr(ctx, obj, "offsetLeft",   JS_NewFloat64(ctx, dr.x));
        JS_SetPropertyStr(ctx, obj, "offsetTop",    JS_NewFloat64(ctx, dr.y));
    }

    // focus() / blur() — no-op stubs
    JS_SetPropertyStr(ctx, obj, "focus",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "focus", 0));
    JS_SetPropertyStr(ctx, obj, "blur",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "blur", 0));
    JS_SetPropertyStr(ctx, obj, "scrollIntoView",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "scrollIntoView", 0));
    JS_SetPropertyStr(ctx, obj, "click",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "click", 0));

    // parentNode / parentElement — direct parent (shallow wrap, no children populated)
    {
        auto par = raw->parent.lock();
        if (par && par->type == NodeType::Element) {
            // Create a shallow wrapper for parent (avoids O(N²) recursion)
            JSValue par_obj = JS_NewObjectClass(ctx, node_class_id);
            JS_SetOpaque(par_obj, par.get());
            JS_SetPropertyStr(ctx, par_obj, "_eng",
                JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));
            std::string par_tag = par->data;
            for (char &c : par_tag) c = (char)::toupper((unsigned char)c);
            JS_SetPropertyStr(ctx, par_obj, "tagName", js_from_string(ctx, par_tag));
            JS_SetPropertyStr(ctx, par_obj, "nodeName", js_from_string(ctx, par_tag));
            JS_SetPropertyStr(ctx, par_obj, "nodeType", JS_NewInt32(ctx, 1));
            auto par_id_it = par->attributes.find("id");
            JS_SetPropertyStr(ctx, par_obj, "id",
                js_from_string(ctx, par_id_it != par->attributes.end() ? par_id_it->second : ""));
            auto par_cls_it = par->attributes.find("class");
            JS_SetPropertyStr(ctx, par_obj, "className",
                js_from_string(ctx, par_cls_it != par->attributes.end() ? par_cls_it->second : ""));
            JS_SetPropertyStr(ctx, par_obj, "children", JS_NewArray(ctx));
            JS_SetPropertyStr(ctx, par_obj, "childNodes", JS_NewArray(ctx));
            JS_SetPropertyStr(ctx, par_obj, "childElementCount",
                JS_NewInt32(ctx, (int)par->children.size()));
            JS_SetPropertyStr(ctx, par_obj, "parentNode", JS_NULL);
            JS_SetPropertyStr(ctx, par_obj, "parentElement", JS_NULL);
            JS_SetPropertyStr(ctx, par_obj, "firstChild", JS_NULL);
            JS_SetPropertyStr(ctx, par_obj, "lastChild", JS_NULL);
            JS_SetPropertyStr(ctx, par_obj, "nextElementSibling", JS_NULL);
            JS_SetPropertyStr(ctx, par_obj, "previousElementSibling", JS_NULL);
            JS_SetPropertyStr(ctx, par_obj, "getAttribute",
                JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av) -> JSValue {
                    if (ac < 1) return JS_NULL;
                    void *p2 = JS_GetOpaque(this_v, 1);
                    if (!p2) return JS_NULL;
                    Node *nd = static_cast<Node*>(p2);
                    std::string name = js_to_string(c, av[0]);
                    auto it = nd->attributes.find(name);
                    return it != nd->attributes.end() ? js_from_string(c, it->second) : JS_NULL;
                }, "getAttribute", 1));
            JS_SetPropertyStr(ctx, par_obj, "querySelector",
                JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av) -> JSValue {
                    if (ac < 1) return JS_NULL;
                    void *p2 = JS_GetOpaque(this_v, 1);
                    JSValue ep2 = JS_GetPropertyStr(c, this_v, "_eng");
                    int64_t ep_i = 0; JS_ToInt64(c, &ep_i, ep2); JS_FreeValue(c, ep2);
                    QJSEngine *eng2 = reinterpret_cast<QJSEngine*>((uintptr_t)ep_i);
                    if (!p2 || !eng2) return JS_NULL;
                    Node *nd = static_cast<Node*>(p2);
                    // Find a shared_ptr matching this raw node
                    auto nd_sp = find_shared_any(eng2, nd);
                    if (!nd_sp) return JS_NULL;
                    std::string sel = js_to_string(c, av[0]);
                    auto found = nd_sp->query_selector(sel);
                    return found ? wrap_node(c, found, eng2) : JS_NULL;
                }, "querySelector", 1));
            JS_SetPropertyStr(ctx, obj, "parentNode",    JS_DupValue(ctx, par_obj));
            JS_SetPropertyStr(ctx, obj, "parentElement", par_obj);
        } else {
            JS_SetPropertyStr(ctx, obj, "parentNode",    JS_NULL);
            JS_SetPropertyStr(ctx, obj, "parentElement", JS_NULL);
        }
    }

    // children / childNodes — wrap immediate children (not their children = no N² recursion)
    {
        JSValue children  = JS_NewArray(ctx);
        JSValue childNodes = JS_NewArray(ctx);
        uint32_t elem_idx = 0, node_idx = 0;
        std::shared_ptr<Node> first_ch, last_ch;
        for (auto &ch : raw->children) {
            if (!ch) continue;
            if (!first_ch) first_ch = ch;
            last_ch = ch;
            // Shallow-wrap: create a minimal JS object for child without populating its children
            JSValue child_obj = JS_NewObjectClass(ctx, node_class_id);
            JS_SetOpaque(child_obj, ch.get());
            JS_SetPropertyStr(ctx, child_obj, "_eng",
                JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));
            std::string ch_tag = ch->type == NodeType::Element ? ch->data : "#text";
            std::string ch_tag_upper = ch_tag;
            for (char &c : ch_tag_upper) c = (char)::toupper((unsigned char)c);
            JS_SetPropertyStr(ctx, child_obj, "tagName",   js_from_string(ctx, ch_tag_upper));
            JS_SetPropertyStr(ctx, child_obj, "nodeName",  js_from_string(ctx, ch_tag_upper));
            JS_SetPropertyStr(ctx, child_obj, "localName", js_from_string(ctx, ch_tag));
            JS_SetPropertyStr(ctx, child_obj, "nodeType",
                JS_NewInt32(ctx, ch->type == NodeType::Element ? 1 : 3));
            {
                auto id_it = ch->attributes.find("id");
                JS_SetPropertyStr(ctx, child_obj, "id",
                    js_from_string(ctx, id_it != ch->attributes.end() ? id_it->second : ""));
                auto cls_it = ch->attributes.find("class");
                JS_SetPropertyStr(ctx, child_obj, "className",
                    js_from_string(ctx, cls_it != ch->attributes.end() ? cls_it->second : ""));
            }
            std::string ch_txt; collect_text(ch.get(), ch_txt);
            JS_SetPropertyStr(ctx, child_obj, "textContent", js_from_string(ctx, ch_txt));
            JS_SetPropertyStr(ctx, child_obj, "innerText",   js_from_string(ctx, ch_txt));
            JS_SetPropertyStr(ctx, child_obj, "children", JS_NewArray(ctx));
            JS_SetPropertyStr(ctx, child_obj, "childNodes", JS_NewArray(ctx));
            JS_SetPropertyStr(ctx, child_obj, "childElementCount",
                JS_NewInt32(ctx, (int)ch->children.size()));
            JS_SetPropertyStr(ctx, child_obj, "parentNode", JS_NULL);
            JS_SetPropertyStr(ctx, child_obj, "parentElement", JS_NULL);
            JS_SetPropertyStr(ctx, child_obj, "firstChild", JS_NULL);
            JS_SetPropertyStr(ctx, child_obj, "lastChild", JS_NULL);
            JS_SetPropertyStr(ctx, child_obj, "nextElementSibling", JS_NULL);
            JS_SetPropertyStr(ctx, child_obj, "previousElementSibling", JS_NULL);
            JS_SetPropertyStr(ctx, child_obj, "getAttribute",
                JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av) -> JSValue {
                    if (ac < 1) return JS_NULL;
                    void *p2 = JS_GetOpaque(this_v, 1);
                    if (!p2) return JS_NULL;
                    Node *nd = static_cast<Node*>(p2);
                    std::string name = js_to_string(c, av[0]);
                    auto it = nd->attributes.find(name);
                    return it != nd->attributes.end() ? js_from_string(c, it->second) : JS_NULL;
                }, "getAttribute", 1));
            JS_SetPropertyStr(ctx, child_obj, "setAttribute",
                JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av) -> JSValue {
                    if (ac < 2) return JS_UNDEFINED;
                    void *p2 = JS_GetOpaque(this_v, 1);
                    JSValue ep2 = JS_GetPropertyStr(c, this_v, "_eng");
                    int64_t ep_i = 0; JS_ToInt64(c, &ep_i, ep2); JS_FreeValue(c, ep2);
                    QJSEngine *eng2 = reinterpret_cast<QJSEngine*>((uintptr_t)ep_i);
                    if (!p2) return JS_UNDEFINED;
                    Node *nd = static_cast<Node*>(p2);
                    std::string name = js_to_string(c, av[0]);
                    std::string val  = js_to_string(c, av[1]);
                    nd->attributes[name] = val;
                    if (eng2) eng2->dom_dirty = true;
                    return JS_UNDEFINED;
                }, "setAttribute", 2));
            JS_SetPropertyStr(ctx, child_obj, "querySelector",
                JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av) -> JSValue {
                    if (ac < 1) return JS_NULL;
                    void *p2 = JS_GetOpaque(this_v, 1);
                    JSValue ep2 = JS_GetPropertyStr(c, this_v, "_eng");
                    int64_t ep_i = 0; JS_ToInt64(c, &ep_i, ep2); JS_FreeValue(c, ep2);
                    QJSEngine *eng2 = reinterpret_cast<QJSEngine*>((uintptr_t)ep_i);
                    if (!p2 || !eng2) return JS_NULL;
                    Node *nd = static_cast<Node*>(p2);
                    auto nd_sp = find_shared_any(eng2, nd);
                    if (!nd_sp) return JS_NULL;
                    std::string sel = js_to_string(c, av[0]);
                    auto found = nd_sp->query_selector(sel);
                    return found ? wrap_node(c, found, eng2) : JS_NULL;
                }, "querySelector", 1));
            JS_SetPropertyUint32(ctx, childNodes, node_idx++, JS_DupValue(ctx, child_obj));
            if (ch->type == NodeType::Element)
                JS_SetPropertyUint32(ctx, children, elem_idx++, JS_DupValue(ctx, child_obj));
            JS_FreeValue(ctx, child_obj);
        }
        JS_SetPropertyStr(ctx, obj, "children",  children);
        JS_SetPropertyStr(ctx, obj, "childNodes", childNodes);
        JS_SetPropertyStr(ctx, obj, "childElementCount", JS_NewInt32(ctx, (int)elem_idx));
        JS_SetPropertyStr(ctx, obj, "firstChild",
            first_ch ? JS_DupValue(ctx,
                JS_GetPropertyUint32(ctx, childNodes, 0)) : JS_NULL);
        JS_SetPropertyStr(ctx, obj, "lastChild",
            last_ch ? JS_DupValue(ctx,
                JS_GetPropertyUint32(ctx, childNodes, node_idx > 0 ? node_idx - 1 : 0)) : JS_NULL);
    }

    // nextElementSibling / previousElementSibling
    {
        auto par = raw->parent.lock();
        std::shared_ptr<Node> next_elem, prev_elem;
        if (par) {
            bool found = false;
            std::shared_ptr<Node> last_elem_before;
            for (auto &s : par->children) {
                if (!s) continue;
                if (s.get() == raw) { found = true; continue; }
                if (!found && s->type == NodeType::Element)
                    last_elem_before = s;
                if (found && s->type == NodeType::Element && !next_elem)
                    next_elem = s;
            }
            prev_elem = last_elem_before;
        }
        JS_SetPropertyStr(ctx, obj, "nextElementSibling",   JS_NULL);
        JS_SetPropertyStr(ctx, obj, "previousElementSibling", JS_NULL);
        JS_SetPropertyStr(ctx, obj, "nextSibling",     JS_NULL);
        JS_SetPropertyStr(ctx, obj, "previousSibling", JS_NULL);
    }

    // data / value properties (for text / input nodes)
    if (raw->type == NodeType::Text)
        JS_SetPropertyStr(ctx, obj, "data", js_from_string(ctx, raw->data));
    {
        auto vit = raw->attributes.find("value");
        JS_SetPropertyStr(ctx, obj, "value",
            js_from_string(ctx, vit != raw->attributes.end() ? vit->second : ""));
    }

    // ── Common element attribute properties ─────────────────────────────────
    {
        static const char *attr_props[] = {
            "href","src","alt","title","lang","type","name","placeholder",
            "action","method","target","rel","role","for","form","autocomplete",
            "tabIndex","accessKey","accesskey","draggable","spellcheck",nullptr
        };
        for (int i = 0; attr_props[i]; i++) {
            auto it = raw->attributes.find(attr_props[i]);
            JS_SetPropertyStr(ctx, obj, attr_props[i],
                js_from_string(ctx, it != raw->attributes.end() ? it->second : ""));
        }
        // Boolean attributes
        JS_SetPropertyStr(ctx, obj, "disabled",
            JS_NewBool(ctx, raw->attributes.count("disabled") > 0));
        JS_SetPropertyStr(ctx, obj, "checked",
            JS_NewBool(ctx, raw->attributes.count("checked") > 0));
        JS_SetPropertyStr(ctx, obj, "hidden",
            JS_NewBool(ctx, raw->attributes.count("hidden") > 0));
        JS_SetPropertyStr(ctx, obj, "selected",
            JS_NewBool(ctx, raw->attributes.count("selected") > 0));
        JS_SetPropertyStr(ctx, obj, "required",
            JS_NewBool(ctx, raw->attributes.count("required") > 0));
        JS_SetPropertyStr(ctx, obj, "readOnly",
            JS_NewBool(ctx, raw->attributes.count("readonly") > 0 ||
                            raw->attributes.count("readOnly") > 0));
        JS_SetPropertyStr(ctx, obj, "multiple",
            JS_NewBool(ctx, raw->attributes.count("multiple") > 0));
    }

    // scrollTop / scrollLeft / scrollHeight / scrollWidth (static; no real scroll state)
    {
        DOMRect dr2{};
        if (engine && engine->layout_cb) engine->layout_cb(raw, dr2);
        JS_SetPropertyStr(ctx, obj, "scrollTop",    JS_NewFloat64(ctx, 0.0));
        JS_SetPropertyStr(ctx, obj, "scrollLeft",   JS_NewFloat64(ctx, 0.0));
        JS_SetPropertyStr(ctx, obj, "scrollHeight", JS_NewFloat64(ctx, dr2.height));
        JS_SetPropertyStr(ctx, obj, "scrollWidth",  JS_NewFloat64(ctx, dr2.width));
    }

    // outerHTML — wraps innerHTML in the element's own tag
    {
        std::string open_tag = "<" + raw->data;
        for (auto &a : raw->attributes)
            open_tag += " " + a.first + "=\"" + a.second + "\"";
        open_tag += ">";
        std::string inner;
        collect_text(raw, inner);
        std::string outer = open_tag + inner + "</" + raw->data + ">";
        JS_SetPropertyStr(ctx, obj, "outerHTML", js_from_string(ctx, outer));
    }

    // dataset — proxy for data-* attributes
    {
        JSValue ds = JS_NewObject(ctx);
        for (auto &a : raw->attributes) {
            if (a.first.substr(0, 5) == "data-") {
                // Convert data-foo-bar → fooBar (camelCase)
                std::string key;
                bool next_upper = false;
                for (size_t i = 5; i < a.first.size(); i++) {
                    char ch = a.first[i];
                    if (ch == '-') { next_upper = true; continue; }
                    if (next_upper) { ch = (char)::toupper((unsigned char)ch); next_upper = false; }
                    key += ch;
                }
                JS_SetPropertyStr(ctx, ds, key.c_str(), js_from_string(ctx, a.second));
            }
        }
        JS_SetPropertyStr(ctx, obj, "dataset", ds);
    }

    // remove() — removes self from parent
    JS_SetPropertyStr(ctx, obj, "remove",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int, JSValue*) -> JSValue {
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            auto par = nd->parent.lock();
            if (!par) return JS_UNDEFINED;
            par->children.erase(
                std::remove_if(par->children.begin(), par->children.end(),
                    [nd](const std::shared_ptr<Node> &n) { return n.get() == nd; }),
                par->children.end());
            eng->dom_dirty = true;
            return JS_UNDEFINED;
        }, "remove", 0));

    // contains(node) — true if node is a descendant (or self)
    JS_SetPropertyStr(ctx, obj, "contains",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_FALSE;
            Node *self = static_cast<Node*>(JS_GetOpaque(tv, 1));
            Node *other = static_cast<Node*>(JS_GetOpaque(av[0], 1));
            if (!self || !other) return JS_NewBool(c, self == other);
            if (self == other) return JS_TRUE;
            // Walk other's ancestors
            auto cur = other->parent.lock();
            while (cur) {
                if (cur.get() == self) return JS_TRUE;
                cur = cur->parent.lock();
            }
            return JS_FALSE;
        }, "contains", 1));

    // cloneNode(deep) — shallow or deep clone
    JS_SetPropertyStr(ctx, obj, "cloneNode",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_NULL;
            Node *nd = static_cast<Node*>(p);
            std::function<std::shared_ptr<Node>(Node*, bool)> do_clone;
            do_clone = [&](Node *n, bool deep) -> std::shared_ptr<Node> {
                auto cl = std::make_shared<Node>(n->type, n->data);
                cl->attributes = n->attributes;
                if (deep) {
                    for (auto &ch : n->children) {
                        auto ch_cl = do_clone(ch.get(), true);
                        ch_cl->parent = cl;
                        cl->children.push_back(ch_cl);
                    }
                }
                return cl;
            };
            bool deep = (ac >= 1) ? (bool)JS_ToBool(c, av[0]) : false;
            auto clone = do_clone(nd, deep);
            eng->created_nodes.push_back(clone);
            return wrap_node(c, clone, eng);
        }, "cloneNode", 1));

    // before(node) / after(node) / prepend(node) / append(node)
    JS_SetPropertyStr(ctx, obj, "before",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_UNDEFINED;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            Node *newch = static_cast<Node*>(JS_GetOpaque(av[0], 1));
            auto par = nd->parent.lock();
            if (!par || !newch) return JS_UNDEFINED;
            auto new_sp = find_shared_any(eng, newch);
            if (!new_sp) return JS_UNDEFINED;
            auto it = std::find_if(par->children.begin(), par->children.end(),
                [nd](const std::shared_ptr<Node> &n) { return n.get() == nd; });
            par->children.insert(it, new_sp);
            eng->dom_dirty = true;
            return JS_UNDEFINED;
        }, "before", 1));
    JS_SetPropertyStr(ctx, obj, "after",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_UNDEFINED;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            Node *newch = static_cast<Node*>(JS_GetOpaque(av[0], 1));
            auto par = nd->parent.lock();
            if (!par || !newch) return JS_UNDEFINED;
            auto new_sp = find_shared_any(eng, newch);
            if (!new_sp) return JS_UNDEFINED;
            auto it = std::find_if(par->children.begin(), par->children.end(),
                [nd](const std::shared_ptr<Node> &n) { return n.get() == nd; });
            if (it != par->children.end()) ++it;
            par->children.insert(it, new_sp);
            eng->dom_dirty = true;
            return JS_UNDEFINED;
        }, "after", 1));
    JS_SetPropertyStr(ctx, obj, "prepend",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_UNDEFINED;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            Node *newch = static_cast<Node*>(JS_GetOpaque(av[0], 1));
            if (!newch) {
                // String argument — prepend as text node
                std::string txt = js_to_string(c, av[0]);
                auto tn = std::make_shared<Node>(NodeType::Text, txt);
                auto nd_sp = find_shared_any(eng, nd);
                if (nd_sp) tn->parent = nd_sp;
                nd->children.insert(nd->children.begin(), tn);
                eng->created_nodes.push_back(tn);
            } else {
                auto new_sp = find_shared_any(eng, newch);
                if (new_sp) nd->children.insert(nd->children.begin(), new_sp);
            }
            eng->dom_dirty = true;
            return JS_UNDEFINED;
        }, "prepend", 1));
    JS_SetPropertyStr(ctx, obj, "append",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 1) return JS_UNDEFINED;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            Node *newch = static_cast<Node*>(JS_GetOpaque(av[0], 1));
            if (!newch) {
                // String argument — append as text node
                std::string txt = js_to_string(c, av[0]);
                auto tn = std::make_shared<Node>(NodeType::Text, txt);
                auto nd_sp = find_shared_any(eng, nd);
                if (nd_sp) tn->parent = nd_sp;
                nd->children.push_back(tn);
                eng->created_nodes.push_back(tn);
            } else {
                auto new_sp = find_shared_any(eng, newch);
                if (new_sp) nd->children.push_back(new_sp);
            }
            eng->dom_dirty = true;
            return JS_UNDEFINED;
        }, "append", 1));

    // insertAdjacentHTML(position, html) — simplified: only "beforeend" / "afterbegin"
    JS_SetPropertyStr(ctx, obj, "insertAdjacentHTML",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 2) return JS_UNDEFINED;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            std::string pos  = js_to_string(c, av[0]);
            std::string html = js_to_string(c, av[1]);
            auto tn = std::make_shared<Node>(NodeType::Text, html);
            eng->created_nodes.push_back(tn);
            if (pos == "beforeend" || pos == "afterbegin") {
                if (pos == "beforeend") nd->children.push_back(tn);
                else nd->children.insert(nd->children.begin(), tn);
                eng->dom_dirty = true;
            }
            return JS_UNDEFINED;
        }, "insertAdjacentHTML", 2));
    JS_SetPropertyStr(ctx, obj, "insertAdjacentElement",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 2) return JS_NULL;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_NULL;
            Node *nd = static_cast<Node*>(p);
            Node *newch = static_cast<Node*>(JS_GetOpaque(av[1], 1));
            if (!newch) return JS_NULL;
            auto new_sp = find_shared_any(eng, newch);
            if (!new_sp) return JS_NULL;
            std::string pos = js_to_string(c, av[0]);
            if (pos == "beforeend") nd->children.push_back(new_sp);
            else if (pos == "afterbegin") nd->children.insert(nd->children.begin(), new_sp);
            eng->dom_dirty = true;
            return JS_DupValue(c, av[1]);
        }, "insertAdjacentElement", 2));
    JS_SetPropertyStr(ctx, obj, "insertAdjacentText",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
            if (ac < 2) return JS_UNDEFINED;
            void *p = JS_GetOpaque(tv, 1);
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!p || !eng) return JS_UNDEFINED;
            Node *nd = static_cast<Node*>(p);
            std::string txt = js_to_string(c, av[1]);
            auto tn = std::make_shared<Node>(NodeType::Text, txt);
            eng->created_nodes.push_back(tn);
            std::string pos = js_to_string(c, av[0]);
            if (pos == "beforeend") nd->children.push_back(tn);
            else if (pos == "afterbegin") nd->children.insert(nd->children.begin(), tn);
            eng->dom_dirty = true;
            return JS_UNDEFINED;
        }, "insertAdjacentText", 2));

    // getRootNode() — returns documentElement proxy
    JS_SetPropertyStr(ctx, obj, "getRootNode",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int, JSValue*) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, tv, "_eng");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng || !eng->dom_root) return JS_NULL;
            return wrap_node(c, eng->dom_root, eng);
        }, "getRootNode", 0));

    return obj;
}

// ── Global console ─────────────────────────────────────────────────────────
static JSValue js_console_log(JSContext *ctx, JSValue this_val,
                               int argc, JSValue *argv) {
    std::cerr << "[JS] ";
    for (int i = 0; i < argc; i++) {
        if (i > 0) std::cerr << " ";
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (str) { std::cerr << str; JS_FreeCString(ctx, str); }
        else      std::cerr << "[object]";
    }
    std::cerr << "\n";
    return JS_UNDEFINED;
}

// ── setTimeout / setInterval — queues callback for qjs_run_pending_timers() ──
static JSValue js_set_timeout(JSContext *ctx, JSValue /*this_val*/,
                               int argc, JSValue *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_NewInt32(ctx, 0);
    // Retrieve engine pointer stored on the global object
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ep = JS_GetPropertyStr(ctx, global, "_qjs_engine_ptr");
    JS_FreeValue(ctx, global);
    int64_t p = 0;
    JS_ToInt64(ctx, &p, ep);
    JS_FreeValue(ctx, ep);
    QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)p);
    if (!eng) return JS_NewInt32(ctx, 0);
    QJSEngine::PendingTimer t;
    t.id = eng->next_timer_id++;
    t.callback = JS_DupValue(ctx, argv[0]);
    for (int i = 2; i < argc; i++)
        t.extra_args.push_back(JS_DupValue(ctx, argv[i]));
    eng->pending_timers.push_back(std::move(t));
    return JS_NewInt32(ctx, eng->next_timer_id - 1);
}

// ── Set up the global environment ──────────────────────────────────────────
static void setup_globals(JSContext *ctx, QJSEngine *engine) {
    JSValue global = JS_GetGlobalObject(ctx);

    // ── Style Proxy helpers ──────────────────────────────────────────────
    JS_SetPropertyStr(ctx, global, "__style_get",
        JS_NewCFunction(ctx, js__style_get, "__style_get", 2));
    JS_SetPropertyStr(ctx, global, "__style_set",
        JS_NewCFunction(ctx, js__style_set, "__style_set", 4));
    JS_SetPropertyStr(ctx, global, "__innerHTML_set",
        JS_NewCFunction(ctx, js__innerHTML_set, "__innerHTML_set", 3));
    // Evaluate the style proxy factory once — it creates a JS Proxy so that
    // el.style.display = 'block' transparently calls __style_set.
    {
        const char *proxy_factory =
            "function __make_style_proxy(ptr, eng) {"
            "  return new Proxy({_ptr: ptr, _eng: eng}, {"
            "    get: function(t, p) {"
            "      if (p === 'setProperty') return function(n,v) { __style_set(ptr,eng,n,v); };"
            "      if (p === 'removeProperty') return function(n) { __style_set(ptr,eng,n,''); };"
            "      if (p === 'cssText') return '';"
            "      return __style_get(ptr, p);"
            "    },"
            "    set: function(t, p, v) { __style_set(ptr, eng, p, v); return true; }"
            "  });"
            "}";
        JSValue r = JS_Eval(ctx, proxy_factory, strlen(proxy_factory),
                            "<style_proxy>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, r);
    }

    // console
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
        JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
        JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    // setTimeout / setInterval (stubs)
    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, js_set_timeout, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) {
            return JS_UNDEFINED;
        }, "clearTimeout", 1));

    // Store engine pointer on global so setTimeout can find it
    JS_SetPropertyStr(ctx, global, "_qjs_engine_ptr",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));

    // clearInterval (same as clearTimeout)
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED;
        }, "clearInterval", 1));

    // window.alert / confirm / prompt
    JS_SetPropertyStr(ctx, global, "alert",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int argc, JSValue *argv) -> JSValue {
            std::string msg = argc >= 1 ? js_to_string(c, argv[0]) : "";
            MessageBoxA(NULL, msg.c_str(), "Alert", MB_OK | MB_ICONINFORMATION);
            return JS_UNDEFINED;
        }, "alert", 1));
    JS_SetPropertyStr(ctx, global, "confirm",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int argc, JSValue *argv) -> JSValue {
            std::string msg = argc >= 1 ? js_to_string(c, argv[0]) : "";
            int r = MessageBoxA(NULL, msg.c_str(), "Confirm", MB_YESNO | MB_ICONQUESTION);
            return JS_NewBool(c, r == IDYES);
        }, "confirm", 1));
    JS_SetPropertyStr(ctx, global, "prompt",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
            return JS_NewString(c, ""); // always returns empty string
        }, "prompt", 1));

    // window.getComputedStyle — returns object with CSS properties as empty strings
    JS_SetPropertyStr(ctx, global, "getComputedStyle",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int argc, JSValue *argv) -> JSValue {
            JSValue style = JS_NewObject(c);
            // If the element has a _ptr, try reading inline style attr
            std::string inline_style;
            if (argc >= 1) {
                JSValue ptr_v = JS_GetPropertyStr(c, argv[0], "_eng");
                int64_t ep = 0; JS_ToInt64(c, &ep, ptr_v); JS_FreeValue(c, ptr_v);
                QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ep);
                JSValue opaque_check = JS_GetPropertyStr(c, argv[0], "tagName");
                JS_FreeValue(c, opaque_check);
                // Read from opaque node pointer
                void *raw_ptr = JS_GetOpaque(argv[0], 1);
                if (raw_ptr) {
                    Node *nd = static_cast<Node*>(raw_ptr);
                    auto it = nd->attributes.find("style");
                    if (it != nd->attributes.end()) inline_style = it->second;
                }
            }
            // getPropertyValue returns empty string (best-effort)
            JS_SetPropertyStr(c, style, "getPropertyValue",
                JS_NewCFunction(c, [](JSContext *c2, JSValue, int, JSValue*) -> JSValue {
                    return JS_NewString(c2, "");
                }, "getPropertyValue", 1));
            // Common properties
            static const char *props[] = {
                "display","visibility","width","height","margin","padding",
                "color","backgroundColor","fontSize","fontFamily","position",
                "top","left","right","bottom","zIndex","overflow","float",
                "textAlign","opacity","transform","transition","flexDirection",
                "alignItems","justifyContent","flexWrap","flex","border",
                "borderRadius","cursor","pointerEvents","boxSizing",nullptr
            };
            for (int i = 0; props[i]; i++)
                JS_SetPropertyStr(c, style, props[i], JS_NewString(c, ""));
            return style;
        }, "getComputedStyle", 1));

    // window.matchMedia — parses (min-width: Npx) / (max-width: Npx)
    JS_SetPropertyStr(ctx, global, "matchMedia",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int argc, JSValue *argv) -> JSValue {
            bool matches = false;
            if (argc >= 1) {
                std::string q = js_to_string(c, argv[0]);
                extern int buffer_width;
                extern int g_viewport_height;
                int vw = buffer_width > 0 ? buffer_width : 800;
                int vh = g_viewport_height > 0 ? g_viewport_height : 600;
                // Match (min-width: Npx)
                size_t p = q.find("min-width");
                if (p != std::string::npos) {
                    size_t colon = q.find(':', p);
                    if (colon != std::string::npos) {
                        try {
                            float n = std::stof(q.substr(colon + 1));
                            matches = (vw >= (int)n);
                        } catch (...) {}
                    }
                }
                // Match (max-width: Npx)
                p = q.find("max-width");
                if (p != std::string::npos) {
                    size_t colon = q.find(':', p);
                    if (colon != std::string::npos) {
                        try {
                            float n = std::stof(q.substr(colon + 1));
                            matches = (vw <= (int)n);
                        } catch (...) {}
                    }
                }
                // Match (prefers-color-scheme: dark) — always false
                if (q.find("prefers-color-scheme") != std::string::npos)
                    matches = false;
            }
            JSValue mq = JS_NewObject(c);
            JS_SetPropertyStr(c, mq, "matches", JS_NewBool(c, matches));
            JS_SetPropertyStr(c, mq, "media",
                argc >= 1 ? JS_DupValue(c, argv[0]) : JS_NewString(c, ""));
            JS_SetPropertyStr(c, mq, "addListener",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "addListener", 1));
            JS_SetPropertyStr(c, mq, "removeListener",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "removeListener", 1));
            JS_SetPropertyStr(c, mq, "addEventListener",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "addEventListener", 2));
            return mq;
        }, "matchMedia", 1));

    // location — seeded with current page URL; updated by qjs_set_page_url
    {
        const std::string &url = engine->page_url;
        // Parse hostname from URL
        std::string hostname, pathname = "/", protocol = "https:", search = "";
        size_t sch = url.find("://");
        if (sch != std::string::npos) {
            protocol = url.substr(0, sch + 1); // "https:"
            size_t host_start = sch + 3;
            size_t slash = url.find('/', host_start);
            hostname = (slash == std::string::npos)
                        ? url.substr(host_start)
                        : url.substr(host_start, slash - host_start);
            if (slash != std::string::npos) {
                size_t q = url.find('?', slash);
                pathname = (q == std::string::npos)
                            ? url.substr(slash)
                            : url.substr(slash, q - slash);
                if (q != std::string::npos) search = url.substr(q);
            }
        }
        JSValue location = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, location, "href",     js_from_string(ctx, url));
        JS_SetPropertyStr(ctx, location, "hostname", js_from_string(ctx, hostname));
        JS_SetPropertyStr(ctx, location, "host",     js_from_string(ctx, hostname));
        JS_SetPropertyStr(ctx, location, "origin",   js_from_string(ctx, protocol + "//" + hostname));
        JS_SetPropertyStr(ctx, location, "protocol", js_from_string(ctx, protocol));
        JS_SetPropertyStr(ctx, location, "pathname", js_from_string(ctx, pathname));
        JS_SetPropertyStr(ctx, location, "search",   js_from_string(ctx, search));
        JS_SetPropertyStr(ctx, location, "hash",     JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, location, "assign",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "assign", 1));
        JS_SetPropertyStr(ctx, location, "replace",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "replace", 1));
        JS_SetPropertyStr(ctx, location, "reload",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "reload", 0));
        JS_SetPropertyStr(ctx, global, "location", location);
    }

    // navigator
    {
        JSValue nav = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, nav, "userAgent",
            JS_NewString(ctx, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                              "AppleWebKit/537.36 (KHTML, like Gecko) "
                              "Chrome/120.0.0.0 Safari/537.36"));
        JS_SetPropertyStr(ctx, nav, "language",   JS_NewString(ctx, "en-US"));
        JS_SetPropertyStr(ctx, nav, "languages",
            JS_Eval(ctx, "['en-US','en']", 14, "<nav>", JS_EVAL_TYPE_GLOBAL));
        JS_SetPropertyStr(ctx, nav, "platform",   JS_NewString(ctx, "Win32"));
        JS_SetPropertyStr(ctx, nav, "vendor",     JS_NewString(ctx, "Google Inc."));
        JS_SetPropertyStr(ctx, nav, "cookieEnabled", JS_FALSE);
        JS_SetPropertyStr(ctx, nav, "onLine",     JS_TRUE);
        JS_SetPropertyStr(ctx, nav, "doNotTrack", JS_NULL);
        JS_SetPropertyStr(ctx, global, "navigator", nav);
    }

    // screen
    {
        JSValue screen = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, screen, "width",       JS_NewInt32(ctx, engine->viewport_width));
        JS_SetPropertyStr(ctx, screen, "height",      JS_NewInt32(ctx, engine->viewport_height));
        JS_SetPropertyStr(ctx, screen, "availWidth",  JS_NewInt32(ctx, engine->viewport_width));
        JS_SetPropertyStr(ctx, screen, "availHeight", JS_NewInt32(ctx, engine->viewport_height));
        JS_SetPropertyStr(ctx, screen, "colorDepth",  JS_NewInt32(ctx, 24));
        JS_SetPropertyStr(ctx, screen, "pixelDepth",  JS_NewInt32(ctx, 24));
        JS_SetPropertyStr(ctx, global, "screen", screen);
    }

    // window size
    JS_SetPropertyStr(ctx, global, "innerWidth",
        JS_NewInt32(ctx, engine->viewport_width));
    JS_SetPropertyStr(ctx, global, "innerHeight",
        JS_NewInt32(ctx, engine->viewport_height));
    JS_SetPropertyStr(ctx, global, "outerWidth",
        JS_NewInt32(ctx, engine->viewport_width));
    JS_SetPropertyStr(ctx, global, "outerHeight",
        JS_NewInt32(ctx, engine->viewport_height));
    JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, global, "scrollY",  JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "scrollX",  JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageYOffset", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, global, "pageXOffset", JS_NewInt32(ctx, 0));

    // history stub
    {
        JSValue hist = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, hist, "length", JS_NewInt32(ctx, 1));
        JS_SetPropertyStr(ctx, hist, "pushState",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "pushState", 3));
        JS_SetPropertyStr(ctx, hist, "replaceState",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "replaceState", 3));
        JS_SetPropertyStr(ctx, hist, "back",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "back", 0));
        JS_SetPropertyStr(ctx, hist, "forward",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "forward", 0));
        JS_SetPropertyStr(ctx, global, "history", hist);
    }

    // localStorage
    {
        auto *eng_ptr = engine;
        JSValue ls = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ls, "_ep",
            JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));
        JS_SetPropertyStr(ctx, ls, "getItem",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
                JSValue ep = JS_GetPropertyStr(c, tv, "_ep");
                int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
                QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
                if (!eng || ac < 1) return JS_NULL;
                std::string key = js_to_string(c, av[0]);
                auto it = eng->local_storage.find(key);
                return it != eng->local_storage.end() ? js_from_string(c, it->second) : JS_NULL;
            }, "getItem", 1));
        JS_SetPropertyStr(ctx, ls, "setItem",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
                JSValue ep = JS_GetPropertyStr(c, tv, "_ep");
                int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
                QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
                if (!eng || ac < 2) return JS_UNDEFINED;
                eng->local_storage[js_to_string(c, av[0])] = js_to_string(c, av[1]);
                return JS_UNDEFINED;
            }, "setItem", 2));
        JS_SetPropertyStr(ctx, ls, "removeItem",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int ac, JSValue *av) -> JSValue {
                JSValue ep = JS_GetPropertyStr(c, tv, "_ep");
                int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
                QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
                if (!eng || ac < 1) return JS_UNDEFINED;
                eng->local_storage.erase(js_to_string(c, av[0]));
                return JS_UNDEFINED;
            }, "removeItem", 1));
        JS_SetPropertyStr(ctx, ls, "clear",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue tv, int, JSValue*) -> JSValue {
                JSValue ep = JS_GetPropertyStr(c, tv, "_ep");
                int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
                QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
                if (eng) eng->local_storage.clear();
                return JS_UNDEFINED;
            }, "clear", 0));
        JS_SetPropertyStr(ctx, global, "localStorage",  ls);
        JS_SetPropertyStr(ctx, global, "sessionStorage", JS_DupValue(ctx, ls));
        (void)eng_ptr;
    }

    // MutationObserver stub
    {
        JSValue mo_ctor = JS_NewCFunction2(ctx,
            [](JSContext *c, JSValue this_val, int, JSValue*) -> JSValue {
                JSValue mo = JS_IsUndefined(this_val) ? JS_NewObject(c) : JS_DupValue(c, this_val);
                JS_SetPropertyStr(c, mo, "observe",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "observe", 2));
                JS_SetPropertyStr(c, mo, "disconnect",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "disconnect", 0));
                JS_SetPropertyStr(c, mo, "takeRecords",
                    JS_NewCFunction(c, [](JSContext *c2, JSValue, int, JSValue*) -> JSValue {
                        return JS_NewArray(c2); }, "takeRecords", 0));
                return mo;
            }, "MutationObserver", 1, JS_CFUNC_constructor_or_func, 0);
        JS_SetPropertyStr(ctx, global, "MutationObserver", mo_ctor);
    }

    // IntersectionObserver stub
    {
        JSValue io_ctor = JS_NewCFunction2(ctx,
            [](JSContext *c, JSValue this_val, int, JSValue*) -> JSValue {
                JSValue io = JS_IsUndefined(this_val) ? JS_NewObject(c) : JS_DupValue(c, this_val);
                JS_SetPropertyStr(c, io, "observe",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "observe", 1));
                JS_SetPropertyStr(c, io, "unobserve",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "unobserve", 1));
                JS_SetPropertyStr(c, io, "disconnect",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "disconnect", 0));
                return io;
            }, "IntersectionObserver", 1, JS_CFUNC_constructor_or_func, 0);
        JS_SetPropertyStr(ctx, global, "IntersectionObserver", io_ctor);
        JS_SetPropertyStr(ctx, global, "ResizeObserver",
            JS_DupValue(ctx, io_ctor)); // same shape
    }

    // window = global
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));

    // document
    JSValue document = JS_NewObject(ctx);

    // document.querySelector
    // Store engine ptr so the lambda can access DOM
    auto *eng_ptr = engine;

    JS_SetPropertyStr(ctx, document, "querySelector",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            // Can't capture in C function; use opaque on document
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t p = 0; JS_ToInt64(c, &p, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)p);
            if (!eng || ac < 1) return JS_NULL;
            std::string sel = js_to_string(c, av[0]);
            auto found = eng->dom_root->query_selector(sel);
            if (!found) return JS_NULL;
            eng->dom_dirty = true;
            return wrap_node(c, found, eng);
        }, "querySelector", 1));

    JS_SetPropertyStr(ctx, document, "querySelectorAll",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t p = 0; JS_ToInt64(c, &p, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)p);
            if (!eng || ac < 1) return JS_NewArray(c);
            std::string sel = js_to_string(c, av[0]);
            auto results = eng->dom_root->query_selector_all(sel);
            JSValue arr = JS_NewArray(c);
            for (uint32_t i = 0; i < results.size(); i++)
                JS_SetPropertyUint32(c, arr, i, wrap_node(c, results[i], eng));
            return arr;
        }, "querySelectorAll", 1));

    JS_SetPropertyStr(ctx, document, "getElementById",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t p = 0; JS_ToInt64(c, &p, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)p);
            if (!eng || ac < 1) return JS_NULL;
            std::string id = "#" + js_to_string(c, av[0]);
            auto found = eng->dom_root->query_selector(id);
            if (!found) return JS_NULL;
            return wrap_node(c, found, eng);
        }, "getElementById", 1));

    JS_SetPropertyStr(ctx, document, "createElement",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av)
                        -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng || ac < 1) return JS_NULL;
            std::string tag = js_to_string(c, av[0]);
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            auto node = std::make_shared<Node>(NodeType::Element, tag);
            eng->created_nodes.push_back(node); // keep alive
            eng->dom_dirty = true;
            return wrap_node(c, node, eng);
        }, "createElement", 1));

    JS_SetPropertyStr(ctx, document, "createTextNode",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av)
                        -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng) return JS_NULL;
            std::string txt = (ac >= 1) ? js_to_string(c, av[0]) : "";
            auto node = std::make_shared<Node>(NodeType::Text, txt);
            eng->created_nodes.push_back(node);
            return wrap_node(c, node, eng);
        }, "createTextNode", 1));

    JS_SetPropertyStr(ctx, document, "createDocumentFragment",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int, JSValue*)
                        -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng) return JS_NULL;
            auto node = std::make_shared<Node>(NodeType::Element,
                                               std::string("#fragment"));
            eng->created_nodes.push_back(node);
            return wrap_node(c, node, eng);
        }, "createDocumentFragment", 0));

    // document.getElementsByTagName(tag)
    JS_SetPropertyStr(ctx, document, "getElementsByTagName",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t p = 0; JS_ToInt64(c, &p, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)p);
            if (!eng || ac < 1) return JS_NewArray(c);
            std::string tag = js_to_string(c, av[0]);
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            std::string sel = tag == "*" ? "*" : tag;
            std::vector<std::shared_ptr<Node>> results;
            if (sel == "*") {
                // All elements — just use querySelectorAll("*") workaround
                results = eng->dom_root->query_selector_all("*");
            } else {
                results = eng->dom_root->query_selector_all(sel);
            }
            JSValue arr = JS_NewArray(c);
            for (uint32_t i = 0; i < results.size(); i++)
                JS_SetPropertyUint32(c, arr, i, wrap_node(c, results[i], eng));
            JS_SetPropertyStr(c, arr, "item",
                JS_NewCFunction(c, [](JSContext *c2, JSValue this2, int ac2, JSValue *av2) -> JSValue {
                    if (ac2 < 1) return JS_UNDEFINED;
                    int32_t idx = 0; JS_ToInt32(c2, &idx, av2[0]);
                    return JS_GetPropertyUint32(c2, this2, (uint32_t)idx);
                }, "item", 1));
            return arr;
        }, "getElementsByTagName", 1));

    // document.getElementsByClassName(cls)
    JS_SetPropertyStr(ctx, document, "getElementsByClassName",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v,
                                int ac, JSValue *av) -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t p = 0; JS_ToInt64(c, &p, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)p);
            if (!eng || ac < 1) return JS_NewArray(c);
            std::string cls = js_to_string(c, av[0]);
            // Build a CSS class selector (handles single class name)
            std::string sel = "." + cls;
            auto results = eng->dom_root->query_selector_all(sel);
            JSValue arr = JS_NewArray(c);
            for (uint32_t i = 0; i < results.size(); i++)
                JS_SetPropertyUint32(c, arr, i, wrap_node(c, results[i], eng));
            return arr;
        }, "getElementsByClassName", 1));

    // document.title getter/setter
    JS_SetPropertyStr(ctx, document, "title",
        JS_NewString(ctx, ""));
    // title setter — updates the tab title via a global string
    JS_SetPropertyStr(ctx, document, "createEvent",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
            JSValue ev = JS_NewObject(c);
            JS_SetPropertyStr(c, ev, "type", JS_NewString(c, ""));
            JS_SetPropertyStr(c, ev, "target", JS_NULL);
            JS_SetPropertyStr(c, ev, "currentTarget", JS_NULL);
            JS_SetPropertyStr(c, ev, "bubbles", JS_FALSE);
            JS_SetPropertyStr(c, ev, "cancelable", JS_FALSE);
            JS_SetPropertyStr(c, ev, "initEvent",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "initEvent", 3));
            JS_SetPropertyStr(c, ev, "preventDefault",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "preventDefault", 0));
            JS_SetPropertyStr(c, ev, "stopPropagation",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "stopPropagation", 0));
            return ev;
        }, "createEvent", 1));

    // Event / CustomEvent constructors
    {
        const char *event_ctor_src =
            "function Event(type, opts) {"
            "  this.type = type || '';"
            "  this.bubbles = (opts && opts.bubbles) || false;"
            "  this.cancelable = (opts && opts.cancelable) || false;"
            "  this.target = null; this.currentTarget = null;"
            "  this.defaultPrevented = false;"
            "  this.preventDefault = function() { this.defaultPrevented = true; };"
            "  this.stopPropagation = function() {};"
            "  this.stopImmediatePropagation = function() {};"
            "}"
            "function CustomEvent(type, opts) {"
            "  Event.call(this, type, opts);"
            "  this.detail = (opts && opts.detail) || null;"
            "}";
        JSValue r = JS_Eval(ctx, event_ctor_src, strlen(event_ctor_src),
                            "<event_ctor>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, r);
    }

    // document.body / document.head
    if (engine->dom_root) {
        auto body = engine->dom_root->query_selector("body");
        auto head = engine->dom_root->query_selector("head");
        if (body) JS_SetPropertyStr(ctx, document, "body",
                      wrap_node(ctx, body, engine));
        if (head) JS_SetPropertyStr(ctx, document, "head",
                      wrap_node(ctx, head, engine));
        JS_SetPropertyStr(ctx, document, "documentElement",
            wrap_node(ctx, engine->dom_root, engine));
    }

    // Store engine pointer in document for callbacks
    JS_SetPropertyStr(ctx, document, "_engine",
        JS_NewInt64(ctx, (int64_t)(uintptr_t)engine));

    JS_SetPropertyStr(ctx, global, "document", document);

    // self = window (Web Workers / general browser compat)
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));

    // addEventListener / removeEventListener on window — no-op stubs
    JS_SetPropertyStr(ctx, global, "addEventListener",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED;
        }, "addEventListener", 2));
    JS_SetPropertyStr(ctx, global, "removeEventListener",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED;
        }, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, global, "dispatchEvent",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_FALSE;
        }, "dispatchEvent", 1));

    // requestAnimationFrame — no-op (no render loop)
    JS_SetPropertyStr(ctx, global, "requestAnimationFrame",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
            return JS_NewInt32(c, 0);
        }, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, global, "cancelAnimationFrame",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED;
        }, "cancelAnimationFrame", 1));

    // performance.now() — real elapsed ms since process start
    {
        JSValue perf = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, perf, "now",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
                LARGE_INTEGER freq, cnt;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&cnt);
                double ms = (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
                return JS_NewFloat64(c, ms);
            }, "now", 0));
        JS_SetPropertyStr(ctx, perf, "mark",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "mark", 1));
        JS_SetPropertyStr(ctx, perf, "measure",
            JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                return JS_UNDEFINED; }, "measure", 1));
        JS_SetPropertyStr(ctx, perf, "getEntriesByName",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
                return JS_NewArray(c); }, "getEntriesByName", 1));
        JS_SetPropertyStr(ctx, global, "performance", perf);
    }

    // XMLHttpRequest stub — callable with 'new'
    {
        // Use JS_NewCFunction2 with JS_CFUNC_constructor so 'new XHR()' works
        JSValue xhr_ctor = JS_NewCFunction2(ctx,
            [](JSContext *c, JSValue this_val, int, JSValue*) -> JSValue {
                // When called with 'new', populate 'this' directly
                JSValue xhr = JS_IsUndefined(this_val) ? JS_NewObject(c) : JS_DupValue(c, this_val);
                JS_SetPropertyStr(c, xhr, "open",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "open", 3));
                JS_SetPropertyStr(c, xhr, "send",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "send", 1));
                JS_SetPropertyStr(c, xhr, "abort",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "abort", 0));
                JS_SetPropertyStr(c, xhr, "setRequestHeader",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "setRequestHeader", 2));
                JS_SetPropertyStr(c, xhr, "addEventListener",
                    JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                        return JS_UNDEFINED; }, "addEventListener", 2));
                JS_SetPropertyStr(c, xhr, "readyState",   JS_NewInt32(c, 0));
                JS_SetPropertyStr(c, xhr, "status",       JS_NewInt32(c, 0));
                JS_SetPropertyStr(c, xhr, "responseText", JS_NewString(c, ""));
                JS_SetPropertyStr(c, xhr, "onload",       JS_NULL);
                JS_SetPropertyStr(c, xhr, "onerror",      JS_NULL);
                JS_SetPropertyStr(c, xhr, "onreadystatechange", JS_NULL);
                return xhr;
            }, "XMLHttpRequest", 0,
            JS_CFUNC_constructor_or_func, 0);
        JS_SetPropertyStr(ctx, global, "XMLHttpRequest", xhr_ctor);
    }

    // fetch stub — returns a Promise-like object that never resolves
    JS_SetPropertyStr(ctx, global, "fetch",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
            // Return a simple object with .then() / .catch() no-ops
            JSValue p = JS_NewObject(c);
            JS_SetPropertyStr(c, p, "then",
                JS_NewCFunction(c, [](JSContext *c2, JSValue, int, JSValue*) -> JSValue {
                    JSValue p2 = JS_NewObject(c2);
                    JS_SetPropertyStr(c2, p2, "catch",
                        JS_NewCFunction(c2, [](JSContext *c3, JSValue, int, JSValue*)
                                        -> JSValue { return JS_UNDEFINED; },
                                        "catch", 1));
                    return p2;
                }, "then", 1));
            JS_SetPropertyStr(c, p, "catch",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "catch", 1));
            return p;
        }, "fetch", 1));

    // document extra stubs
    JS_SetPropertyStr(ctx, document, "readyState",
        JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, document, "cookie",
        JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, document, "addEventListener",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED;
        }, "addEventListener", 2));
    JS_SetPropertyStr(ctx, document, "removeEventListener",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED;
        }, "removeEventListener", 2));

    // document.activeElement — returns body
    if (engine->dom_root) {
        auto body = engine->dom_root->query_selector("body");
        JS_SetPropertyStr(ctx, document, "activeElement",
            body ? wrap_node(ctx, body, engine) : JS_NULL);
    } else {
        JS_SetPropertyStr(ctx, document, "activeElement", JS_NULL);
    }

    // document visibility
    JS_SetPropertyStr(ctx, document, "hidden",           JS_FALSE);
    JS_SetPropertyStr(ctx, document, "visibilityState",  JS_NewString(ctx, "visible"));
    JS_SetPropertyStr(ctx, document, "hasFocus",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
            return JS_TRUE; }, "hasFocus", 0));
    JS_SetPropertyStr(ctx, document, "referrer",    JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, document, "defaultView", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, document, "characterSet", JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, document, "charset",      JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, document, "contentType",  JS_NewString(ctx, "text/html"));
    JS_SetPropertyStr(ctx, document, "domain",       JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, document, "URL",
        JS_NewString(ctx, engine->page_url.c_str()));
    JS_SetPropertyStr(ctx, document, "baseURI",
        JS_NewString(ctx, engine->page_url.c_str()));
    JS_SetPropertyStr(ctx, document, "compatMode",   JS_NewString(ctx, "CSS1Compat"));

    // document.createElementNS(ns, tag) — ignores namespace
    JS_SetPropertyStr(ctx, document, "createElementNS",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av)
                        -> JSValue {
            // same as createElement, ignore namespace
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng || ac < 2) return JS_NULL;
            std::string tag = js_to_string(c, av[1]);
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            // Strip namespace prefix (e.g. "svg:circle" → "circle")
            auto colon = tag.find(':');
            if (colon != std::string::npos) tag = tag.substr(colon + 1);
            auto node = std::make_shared<Node>(NodeType::Element, tag);
            eng->created_nodes.push_back(node);
            eng->dom_dirty = true;
            return wrap_node(c, node, eng);
        }, "createElementNS", 2));

    // document.createComment(text)
    JS_SetPropertyStr(ctx, document, "createComment",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av)
                        -> JSValue {
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng) return JS_NULL;
            std::string txt = (ac >= 1) ? js_to_string(c, av[0]) : "";
            auto node = std::make_shared<Node>(NodeType::Text, "<!--" + txt + "-->");
            eng->created_nodes.push_back(node);
            return wrap_node(c, node, eng);
        }, "createComment", 1));

    // document.importNode(node, deep) — returns a clone
    JS_SetPropertyStr(ctx, document, "importNode",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue this_v, int ac, JSValue *av)
                        -> JSValue {
            if (ac < 1) return JS_NULL;
            JSValue ep = JS_GetPropertyStr(c, this_v, "_engine");
            int64_t ei = 0; JS_ToInt64(c, &ei, ep); JS_FreeValue(c, ep);
            QJSEngine *eng = reinterpret_cast<QJSEngine*>((uintptr_t)ei);
            if (!eng) return JS_NULL;
            Node *nd = static_cast<Node*>(JS_GetOpaque(av[0], 1));
            if (!nd) return JS_NULL;
            bool deep = (ac >= 2) ? (bool)JS_ToBool(c, av[1]) : false;
            std::function<std::shared_ptr<Node>(Node*, bool)> do_clone;
            do_clone = [&](Node *n, bool d) -> std::shared_ptr<Node> {
                auto cl = std::make_shared<Node>(n->type, n->data);
                cl->attributes = n->attributes;
                if (d) for (auto &ch : n->children) {
                    auto cc = do_clone(ch.get(), true);
                    cc->parent = cl;
                    cl->children.push_back(cc);
                }
                return cl;
            };
            auto clone = do_clone(nd, deep);
            eng->created_nodes.push_back(clone);
            return wrap_node(c, clone, eng);
        }, "importNode", 2));

    // window.scrollTo / scroll / scrollBy
    JS_SetPropertyStr(ctx, global, "scrollTo",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "scrollTo", 2));
    JS_SetPropertyStr(ctx, global, "scroll",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "scroll", 2));
    JS_SetPropertyStr(ctx, global, "scrollBy",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "scrollBy", 2));

    // window.getSelection() — stub
    JS_SetPropertyStr(ctx, global, "getSelection",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
            JSValue sel = JS_NewObject(c);
            JS_SetPropertyStr(c, sel, "toString",
                JS_NewCFunction(c, [](JSContext *c2, JSValue, int, JSValue*) -> JSValue {
                    return JS_NewString(c2, ""); }, "toString", 0));
            JS_SetPropertyStr(c, sel, "removeAllRanges",
                JS_NewCFunction(c, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
                    return JS_UNDEFINED; }, "removeAllRanges", 0));
            JS_SetPropertyStr(c, sel, "rangeCount", JS_NewInt32(c, 0));
            return sel;
        }, "getSelection", 0));

    // window.open() — stub (returns null)
    JS_SetPropertyStr(ctx, global, "open",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_NULL; }, "open", 3));

    // window.print() — no-op
    JS_SetPropertyStr(ctx, global, "print",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "print", 0));

    // window.focus() / window.blur() — no-op
    JS_SetPropertyStr(ctx, global, "focus",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "focus", 0));
    JS_SetPropertyStr(ctx, global, "blur",
        JS_NewCFunction(ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "blur", 0));

    // CSS.supports() stub
    {
        JSValue css = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, css, "supports",
            JS_NewCFunction(ctx, [](JSContext *c, JSValue, int, JSValue*) -> JSValue {
                return JS_FALSE; }, "supports", 2));
        JS_SetPropertyStr(ctx, global, "CSS", css);
    }

    // queueMicrotask — run immediately via Promise.resolve().then()
    JS_SetPropertyStr(ctx, global, "queueMicrotask",
        JS_NewCFunction(ctx, [](JSContext *c, JSValue, int ac, JSValue *av) -> JSValue {
            if (ac < 1 || !JS_IsFunction(c, av[0])) return JS_UNDEFINED;
            // Just call it immediately (microtask timing approximation)
            JSValue ret = JS_Call(c, av[0], JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(c, ret);
            return JS_UNDEFINED;
        }, "queueMicrotask", 1));

    JS_FreeValue(ctx, global);
}

// ── Public API ─────────────────────────────────────────────────────────────

QJSEngine* qjs_create(std::shared_ptr<Node> dom_root) {
    // Register node class (once)
    if (!node_class_id)
        JS_NewClassID(&node_class_id);

    auto *engine = new QJSEngine();
    engine->dom_root = dom_root;

    engine->rt  = JS_NewRuntime();
    JS_SetMemoryLimit(engine->rt, 128 * 1024 * 1024); // 128MB heap
    JS_SetMaxStackSize(engine->rt, 4 * 1024 * 1024);  // 4MB JS call stack (prevents SO)

    engine->ctx = JS_NewContext(engine->rt);
    JS_NewClass(engine->rt, node_class_id, &node_class_def);

    setup_globals(engine->ctx, engine);
    return engine;
}

void qjs_destroy(QJSEngine *engine) {
    if (!engine) return;
    // Free all stored event listener JSValues before destroying context
    for (auto &node_map : engine->event_listeners) {
        for (auto &type_vec : node_map.second) {
            for (auto &fn : type_vec.second) {
                JS_FreeValue(engine->ctx, fn);
            }
        }
    }
    engine->event_listeners.clear();
    if (engine->ctx) JS_FreeContext(engine->ctx);
    if (engine->rt)  JS_FreeRuntime(engine->rt);
    delete engine;
}

std::string qjs_eval(QJSEngine *engine, const std::string& source,
                     const std::string& filename) {
    if (!engine || source.empty()) return "";

    // Hard cap: kill any script that runs longer than 1s
    g_js_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    JS_SetInterruptHandler(engine->rt, js_interrupt_handler, nullptr);

    JSValue result = JS_Eval(engine->ctx,
        source.c_str(), source.size(),
        filename.c_str(),
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);

    // Clear handler so it doesn't fire during DOM work after eval
    JS_SetInterruptHandler(engine->rt, nullptr, nullptr);

    std::string error;
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(engine->ctx);
        JSValue msg = JS_GetPropertyStr(engine->ctx, ex, "message");
        error = js_to_string(engine->ctx, msg);
        JS_FreeValue(engine->ctx, msg);
        JS_FreeValue(engine->ctx, ex);
        // Treat a timeout as a warning, not a hard error
        if (error.find("interrupted") != std::string::npos) {
            std::cerr << "[JS TIMEOUT] script exceeded limit, killed\n";
            error.clear();
        }
    }

    JS_FreeValue(engine->ctx, result);

    // Pump the microtask queue — this resolves Promises, async/await chains, etc.
    {
        JSContext *ctx2 = nullptr;
        int max_jobs = 4096;
        while (max_jobs-- > 0) {
            int r = JS_ExecutePendingJob(engine->rt, &ctx2);
            if (r <= 0) break;
        }
    }

    engine->dom_dirty = true; // assume DOM may have changed
    return error;
}

void qjs_run_scripts(QJSEngine *engine, std::shared_ptr<Node> root) {
    if (!engine || !root) return;

    auto scripts_start = std::chrono::steady_clock::now();
    bool budget_exceeded = false;

    std::function<void(std::shared_ptr<Node>)> run;
    run = [&](std::shared_ptr<Node> node) {
        if (!node || budget_exceeded) return;
        if (node->type == NodeType::Element && node->data == "script") {
            // Check total time budget (8 seconds for all scripts combined)
            auto elapsed = std::chrono::steady_clock::now() - scripts_start;
            if (elapsed > std::chrono::milliseconds(8000)) {
                std::cerr << "[JS] Total script budget exceeded (8s), skipping remaining\n";
                budget_exceeded = true;
                return;
            }
            // Skip external scripts (src attribute) — we don't fetch them yet
            if (node->attributes.count("src")) {
                return;
            }
            std::string source;
            for (auto& c : node->children)
                if (c && c->type == NodeType::Text)
                    source += c->data;
            // Skip very large scripts (>256KB) — they're usually framework bundles
            // that depend on APIs we don't have and tend to crash/hang QuickJS.
            // Scripts 4-256KB are logged but run (with 1s timeout each).
            if (source.size() > 256 * 1024) {
                std::cerr << "[JS] Skipping oversized script ("
                          << source.size()/1024 << " KB > 256 KB)\n";
                return;
            }
            if (source.size() > 4096) {
                std::cerr << "[JS] Large inline script ("
                          << source.size()/1024 << " KB) — running\n";
            }
            if (!source.empty()) {
                try {
                    std::string err = qjs_eval(engine, source, "<script>");
                    if (!err.empty())
                        std::cerr << "[JS ERROR] " << err << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "[JS CRASH] " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "[JS CRASH] unknown exception in script\n";
                }
            }
            return;
        }
        for (auto& c : node->children) run(c);
    };
    run(root);
    auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - scripts_start).count();
    std::cerr << "Scripts done (" << total << " ms)\n";
}

void qjs_call_global(QJSEngine *engine, const std::string& name) {
    if (!engine) return;
    JSValue global = JS_GetGlobalObject(engine->ctx);
    JSValue fn = JS_GetPropertyStr(engine->ctx, global, name.c_str());
    if (JS_IsFunction(engine->ctx, fn)) {
        JSValue ret = JS_Call(engine->ctx, fn, global, 0, nullptr);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(engine->ctx);
            JSValue msg = JS_GetPropertyStr(engine->ctx, ex, "message");
            std::cerr << "[JS] " << name << "() error: "
                      << js_to_string(engine->ctx, msg) << "\n";
            JS_FreeValue(engine->ctx, msg);
            JS_FreeValue(engine->ctx, ex);
        }
        JS_FreeValue(engine->ctx, ret);
        // Pump microtasks after calling a global function
        JSContext *ctx2 = nullptr;
        int max_jobs = 4096;
        while (max_jobs-- > 0 && JS_ExecutePendingJob(engine->rt, &ctx2) > 0) {}
    }
    JS_FreeValue(engine->ctx, fn);
    JS_FreeValue(engine->ctx, global);
}

bool qjs_run_pending_timers(QJSEngine *engine) {
    if (!engine || engine->pending_timers.empty()) return false;
    // Swap to avoid re-entrant additions causing infinite loops
    std::vector<QJSEngine::PendingTimer> timers;
    timers.swap(engine->pending_timers);
    for (auto &t : timers) {
        JSValue *args_ptr = t.extra_args.empty() ? nullptr : t.extra_args.data();
        JSValue ret = JS_Call(engine->ctx, t.callback,
                              JS_UNDEFINED, (int)t.extra_args.size(), args_ptr);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(engine->ctx);
            std::string msg = js_to_string(engine->ctx, exc);
            std::cerr << "[JS timer] " << msg << "\n";
            JS_FreeValue(engine->ctx, exc);
        }
        JS_FreeValue(engine->ctx, ret);
        JS_FreeValue(engine->ctx, t.callback);
        for (auto &a : t.extra_args) JS_FreeValue(engine->ctx, a);
    }
    // Drain any micro-task queue (Promise .then chains)
    JSContext *ctx2;
    int lim = 2048;
    while (lim-- > 0 && JS_ExecutePendingJob(engine->rt, &ctx2) > 0) {}
    if (engine->dom_dirty) return true;
    return !timers.empty();
}

// ── New public API ──────────────────────────────────────────────────────────

void qjs_fire_event(QJSEngine *engine, Node *node,
                    const std::string& type, int clientX, int clientY) {
    if (!engine || !node) return;
    auto it = engine->event_listeners.find(node);
    if (it == engine->event_listeners.end()) return;
    auto it2 = it->second.find(type);
    if (it2 == it->second.end()) return;

    // Build a minimal Event object
    JSValue ev = JS_NewObject(engine->ctx);
    JS_SetPropertyStr(engine->ctx, ev, "type",     js_from_string(engine->ctx, type));
    JS_SetPropertyStr(engine->ctx, ev, "clientX",  JS_NewInt32(engine->ctx, clientX));
    JS_SetPropertyStr(engine->ctx, ev, "clientY",  JS_NewInt32(engine->ctx, clientY));
    JS_SetPropertyStr(engine->ctx, ev, "pageX",    JS_NewInt32(engine->ctx, clientX));
    JS_SetPropertyStr(engine->ctx, ev, "pageY",    JS_NewInt32(engine->ctx, clientY));
    JS_SetPropertyStr(engine->ctx, ev, "bubbles",  JS_TRUE);
    JS_SetPropertyStr(engine->ctx, ev, "defaultPrevented", JS_FALSE);
    JS_SetPropertyStr(engine->ctx, ev, "preventDefault",
        JS_NewCFunction(engine->ctx, [](JSContext *c, JSValue tv, int, JSValue*) -> JSValue {
            JS_SetPropertyStr(c, tv, "defaultPrevented", JS_TRUE);
            return JS_UNDEFINED;
        }, "preventDefault", 0));
    JS_SetPropertyStr(engine->ctx, ev, "stopPropagation",
        JS_NewCFunction(engine->ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "stopPropagation", 0));
    JS_SetPropertyStr(engine->ctx, ev, "stopImmediatePropagation",
        JS_NewCFunction(engine->ctx, [](JSContext*, JSValue, int, JSValue*) -> JSValue {
            return JS_UNDEFINED; }, "stopImmediatePropagation", 0));
    // target = wrapped node
    auto node_sp = find_shared_any(engine, node);
    if (node_sp) JS_SetPropertyStr(engine->ctx, ev, "target",
                     wrap_node(engine->ctx, node_sp, engine));

    // Call each registered handler
    JSValue global = JS_GetGlobalObject(engine->ctx);
    for (auto &fn : it2->second) {
        if (JS_IsFunction(engine->ctx, fn)) {
            JSValue ret = JS_Call(engine->ctx, fn, global, 1, &ev);
            if (JS_IsException(ret)) {
                JSValue ex = JS_GetException(engine->ctx);
                JS_FreeValue(engine->ctx, ex);
            }
            JS_FreeValue(engine->ctx, ret);
        }
    }
    JS_FreeValue(engine->ctx, global);
    JS_FreeValue(engine->ctx, ev);

    // Pump microtasks
    JSContext *ctx2 = nullptr;
    int max = 4096;
    while (max-- > 0 && JS_ExecutePendingJob(engine->rt, &ctx2) > 0) {}

    engine->dom_dirty = true;
}

void qjs_set_viewport(QJSEngine *engine, int width, int height) {
    if (!engine) return;
    engine->viewport_width  = width;
    engine->viewport_height = height;
    // Update the live JS globals
    if (!engine->ctx) return;
    JSValue global = JS_GetGlobalObject(engine->ctx);
    JS_SetPropertyStr(engine->ctx, global, "innerWidth",  JS_NewInt32(engine->ctx, width));
    JS_SetPropertyStr(engine->ctx, global, "innerHeight", JS_NewInt32(engine->ctx, height));
    JS_SetPropertyStr(engine->ctx, global, "outerWidth",  JS_NewInt32(engine->ctx, width));
    JS_SetPropertyStr(engine->ctx, global, "outerHeight", JS_NewInt32(engine->ctx, height));
    JS_FreeValue(engine->ctx, global);
}

void qjs_set_page_url(QJSEngine *engine, const std::string& url) {
    if (!engine) return;
    engine->page_url = url;
    // Update window.location.href
    if (!engine->ctx) return;
    JSValue global = JS_GetGlobalObject(engine->ctx);
    JSValue loc = JS_GetPropertyStr(engine->ctx, global, "location");
    if (!JS_IsUndefined(loc) && !JS_IsNull(loc))
        JS_SetPropertyStr(engine->ctx, loc, "href", js_from_string(engine->ctx, url));
    JS_FreeValue(engine->ctx, loc);
    JS_FreeValue(engine->ctx, global);
}

void qjs_set_layout_cb(QJSEngine *engine,
                       std::function<bool(Node*, DOMRect&)> cb) {
    if (engine) engine->layout_cb = cb;
}

bool qjs_dom_dirty(QJSEngine *engine) {
    return engine && engine->dom_dirty;
}

void qjs_clear_dirty(QJSEngine *engine) {
    if (engine) engine->dom_dirty = false;
}
