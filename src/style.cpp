#include "css_parser.h"   // for expand_shorthand()
#include "selector.h"
#include "ua_styles.h"
#include "ua_stylesheet.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// Selector matching (Specificity, has_class, parse_anb, match_simple,
// match_inner_selector, RuleIndex, build_rule_index, gather_rules,
// match_selector, matches_rule, match_rules, match_pseudo_element)
// moved to selector.cpp (Split D)

using NodePtr = std::shared_ptr<Node>;


/* =============================
   Style tree
   ============================= */
static std::shared_ptr<StyledNode> style_tree(std::shared_ptr<Node> root,
                                              const Stylesheet &stylesheet,
                                              const RuleIndex &index,
                                              const std::shared_ptr<StyledNode>& parent,
                                              int depth,
                                              const Stylesheet *hover_sheet  = nullptr,
                                              const RuleIndex  *hover_index  = nullptr,
                                              const Stylesheet *focus_sheet  = nullptr,
                                              const RuleIndex  *focus_index  = nullptr) {

  if (!root)
    return nullptr;

  if (depth > 200)
    return std::make_shared<StyledNode>(root);

  auto node = std::make_shared<StyledNode>(root);
  node->parent = parent;

  if (root->type == NodeType::Element) {
    // ── UA default stylesheet (lowest priority) ─────────────────────
    // Apply UA defaults via real CSS selector matching first, so author
    // rules from `stylesheet` naturally overwrite them when they conflict.
    // This is how Gecko's html.css feeds into the cascade.
    {
      const Stylesheet &ua_sheet = get_ua_stylesheet();
      const RuleIndex  &ua_index = get_ua_rule_index();
      node->specified_values = match_rules(root, ua_sheet, ua_index);
    }

    // ── Author stylesheet — overwrites UA where they conflict ───────
    {
      PropertyMap author_vals = match_rules(root, stylesheet, index);
      for (auto &kv : author_vals)
        node->specified_values[kv.first] = kv.second;
    }

    // ── UA force-override rules (script display:none, etc.) ─────────
    // These run AFTER author rules so they always win — author CSS can
    // never make a <script> element visible, etc. See ua_styles.cpp.
    apply_ua_defaults(node.get(), root->data);

    // Apply :hover rules if this element is currently hovered
    if (hover_sheet && hover_index && root->attributes.count("__hover")) {
      PropertyMap hover_vals = match_rules(root, *hover_sheet, *hover_index);
      for (auto& kv : hover_vals)
        node->specified_values[kv.first] = kv.second;
    }
    // Apply :focus rules if this element is focused
    if (focus_sheet && focus_index && root->attributes.count("__focus")) {
      PropertyMap focus_vals = match_rules(root, *focus_sheet, *focus_index);
      for (auto& kv : focus_vals)
        node->specified_values[kv.first] = kv.second;
    }
    
    // Parse inline style attribute. Each declaration is expanded through
    // expand_shorthand() so margin/padding/inset shorthands become longhands
    // immediately — same expansion path as the stylesheet parser. Later
    // declarations overwrite earlier ones (CSS source-order semantics).
    auto it = root->attributes.find("style");
    if (it != root->attributes.end()) {
        std::stringstream ss(it->second);
        std::string decl;
        while (std::getline(ss, decl, ';')) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            std::string name = decl.substr(0, colon);
            std::string value = decl.substr(colon + 1);
            name.erase(0, name.find_first_not_of(" \t\r\n"));
            name.erase(name.find_last_not_of(" \t\r\n") + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            size_t ve = value.find_last_not_of(" \t\r\n");
            if (ve != std::string::npos) value.erase(ve + 1);
            if (name.empty()) continue;

            Declaration d;
            d.name = name;
            d.value = value;
            auto expanded = expand_shorthand(d);

            // For shorthand declarations, also wipe out any previously-set
            // longhands (from an earlier inline declaration) that the new
            // shorthand would cover. This matches CSS source-order semantics:
            // `padding-left: 5px; padding: 20px` → padding-left becomes 20px.
            if (name == "padding") {
                node->specified_values.erase("padding-top");
                node->specified_values.erase("padding-right");
                node->specified_values.erase("padding-bottom");
                node->specified_values.erase("padding-left");
            } else if (name == "margin") {
                node->specified_values.erase("margin-top");
                node->specified_values.erase("margin-right");
                node->specified_values.erase("margin-bottom");
                node->specified_values.erase("margin-left");
            } else if (name == "inset") {
                node->specified_values.erase("top");
                node->specified_values.erase("right");
                node->specified_values.erase("bottom");
                node->specified_values.erase("left");
            }

            for (auto &ed : expanded)
                node->specified_values[ed.name] = ed.value;
        }
    }

    // ── Resolve CSS custom properties (var()) and calc() ──────────────────
    // This runs after both stylesheet rules AND inline styles are applied,
    // so var() references in either context get resolved correctly.
    {
        auto& sv = node->specified_values;

        // ── Step 1: Collect all --custom-property values from this node
        // and walk up the ancestor chain (inheritance).
        std::unordered_map<std::string, std::string> custom_props;

        // Walk ancestors to collect inherited custom properties
        auto ancestor = parent;
        while (ancestor) {
            for (auto& kv : ancestor->specified_values) {
                if (kv.first.size() > 2 && kv.first[0] == '-' && kv.first[1] == '-') {
                    // Don't overwrite — closer ancestor wins (already added)
                    if (!custom_props.count(kv.first))
                        custom_props[kv.first] = kv.second;
                }
            }
            ancestor = ancestor->parent.lock();
        }
        // Current node's own custom props override ancestors
        for (auto& kv : sv) {
            if (kv.first.size() > 2 && kv.first[0] == '-' && kv.first[1] == '-')
                custom_props[kv.first] = kv.second;
        }

        // ── Step 2: Resolve var(--name, fallback) in all property values
        auto resolve_var = [&](const std::string& val) -> std::string {
            if (val.find("var(") == std::string::npos) return val;
            std::string result;
            size_t i = 0;
            while (i < val.size()) {
                size_t vp = val.find("var(", i);
                if (vp == std::string::npos) { result += val.substr(i); break; }
                result += val.substr(i, vp - i);
                // find matching closing paren (handles nested)
                size_t start = vp + 4;
                int depth = 1;
                size_t j = start;
                while (j < val.size() && depth > 0) {
                    if (val[j] == '(') depth++;
                    else if (val[j] == ')') depth--;
                    if (depth > 0) j++;
                    else break;
                }
                std::string inner = val.substr(start, j - start);
                i = j + 1; // past closing )

                // Split inner into name and optional fallback at first comma
                std::string var_name, fallback;
                size_t comma = inner.find(',');
                if (comma != std::string::npos) {
                    var_name = inner.substr(0, comma);
                    fallback = inner.substr(comma + 1);
                    // trim
                    var_name.erase(0, var_name.find_first_not_of(" \t"));
                    var_name.erase(var_name.find_last_not_of(" \t") + 1);
                    fallback.erase(0, fallback.find_first_not_of(" \t"));
                    fallback.erase(fallback.find_last_not_of(" \t") + 1);
                } else {
                    var_name = inner;
                    var_name.erase(0, var_name.find_first_not_of(" \t"));
                    var_name.erase(var_name.find_last_not_of(" \t") + 1);
                }

                auto it = custom_props.find(var_name);
                if (it != custom_props.end() && !it->second.empty())
                    result += it->second;
                else if (!fallback.empty())
                    result += fallback;
                // else: unresolved var() — emit nothing (property will be empty)
            }
            return result;
        };

        // Apply var() resolution to all non-custom properties
        for (auto& kv : sv) {
            if (kv.first.size() > 2 && kv.first[0] == '-' && kv.first[1] == '-')
                continue; // skip custom properties themselves
            if (kv.second.find("var(") != std::string::npos)
                kv.second = resolve_var(kv.second);
        }

        // ── Step 3: Resolve calc() expressions
        // Supports: calc(Apx + Bpx), calc(A% + Bpx), calc(100% - Npx),
        //           calc(N * X), calc(A / B), nested calc()
        auto resolve_calc = [&](const std::string& val,
                                float percentage_basis) -> std::string {
            if (val.find("calc(") == std::string::npos) return val;

            // Find and replace each calc(...) occurrence
            std::string result;
            size_t i = 0;
            while (i < val.size()) {
                size_t cp = val.find("calc(", i);
                if (cp == std::string::npos) { result += val.substr(i); break; }
                result += val.substr(i, cp - i);
                size_t start = cp + 5;
                int depth = 1;
                size_t j = start;
                while (j < val.size() && depth > 0) {
                    if (val[j] == '(') depth++;
                    else if (val[j] == ')') depth--;
                    if (depth > 0) j++;
                    else break;
                }
                std::string expr = val.substr(start, j - start);
                i = j + 1;

                // Tokenize and evaluate: supports +, -, *, /
                // Convert tokens to px values then compute
                auto to_px = [&](const std::string& tok) -> float {
                    if (tok.empty()) return 0.f;
                    try {
                        if (tok.size() > 2 && tok.substr(tok.size()-2) == "px")
                            return std::stof(tok);
                        if (tok.size() > 2 && tok.substr(tok.size()-2) == "em")
                            return std::stof(tok) * 16.f; // assume 16px base
                        if (tok.size() > 3 && tok.substr(tok.size()-3) == "rem")
                            return std::stof(tok) * 16.f;
                        if (!tok.empty() && tok.back() == '%')
                            return std::stof(tok) * percentage_basis / 100.f;
                        return std::stof(tok); // bare number
                    } catch (...) { return 0.f; }
                };

                // Simple left-to-right evaluator (handles +/-/* //)
                // Tokenize by spaces around operators
                std::vector<std::string> tokens;
                std::string tok;
                for (char c : expr) {
                    if (c == ' ') {
                        if (!tok.empty()) { tokens.push_back(tok); tok.clear(); }
                    } else {
                        tok += c;
                    }
                }
                if (!tok.empty()) tokens.push_back(tok);

                float acc = 0.f;
                char op = '+';
                for (auto& t : tokens) {
                    if (t == "+" || t == "-" || t == "*" || t == "/") {
                        op = t[0];
                    } else {
                        float v = to_px(t);
                        switch (op) {
                            case '+': acc += v; break;
                            case '-': acc -= v; break;
                            case '*': acc *= v; break;
                            case '/': if (v != 0) acc /= v; break;
                        }
                        op = '+'; // reset to + after first operand consumed
                    }
                }
                // Emit as px
                result += std::to_string((int)acc) + "px";
            }
            return result;
        };

        // Apply calc() to dimensional properties
        // Use 100% as default percentage basis (layout will refine later)
        static const char* calc_props[] = {
            "width", "height", "min-width", "min-height", "max-width", "max-height",
            "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
            "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
            "top", "right", "bottom", "left", "gap", "font-size", nullptr
        };
        for (int pi = 0; calc_props[pi]; pi++) {
            auto it = sv.find(calc_props[pi]);
            if (it != sv.end() && it->second.find("calc(") != std::string::npos) {
                // Only eagerly resolve calc() that has NO % — those need
                // the real containing-block width available at layout time.
                // layout.cpp's parse_px() handles % calc() at layout time.
                if (it->second.find('%') == std::string::npos)
                    it->second = resolve_calc(it->second, 100.f);
                // else: leave as calc(...) — parse_px will resolve with real width
            }
        }
    }

    // ── Other shorthands & logical-property mappings ─────────────────────
    // margin/padding/inset are now expanded at parse time by expand_shorthand()
    // (called from CSSParser::parse_declarations and the inline-style parser
    // above), so they never reach this point as shorthand keys.
    auto& sv = node->specified_values;

    // Helper: split a shorthand value into up to 4 space-separated tokens
    auto split_vals = [](const std::string& v) {
        std::vector<std::string> parts;
        std::stringstream ss(v);
        std::string t;
        while (ss >> t) parts.push_back(t);
        return parts;
    };

    // font shorthand: extract font-size (and font-family if present)
    // Format: [style] [weight] size[/line-height] family
    if (sv.count("font") && !sv.count("font-size")) {
        auto p = split_vals(sv["font"]);
        for (const auto& tok : p) {
            if (tok.back() == '%' || (tok.find("px") != std::string::npos)
                || (tok.find("em") != std::string::npos)
                || (tok.find("pt") != std::string::npos)) {
                // strip /line-height
                std::string sz = tok.substr(0, tok.find('/'));
                sv["font-size"] = sz;
                break;
            }
            if (tok == "bold" || tok == "bolder") sv["font-weight"] = "bold";
            if (tok == "italic") sv["font-style"] = "italic";
        }
    }

    // background shorthand: if background-color not set, try to extract a color
    if (sv.count("background") && !sv.count("background-color")) {
        const std::string& bg = sv["background"];
        // If the background value is a gradient, store it as background-image too
        if (!sv.count("background-image") &&
            (bg.find("linear-gradient") != std::string::npos ||
             bg.find("radial-gradient") != std::string::npos ||
             bg.find("conic-gradient") != std::string::npos)) {
            sv["background-image"] = bg;
        }
        auto p = split_vals(bg);
        // Known CSS color keywords (subset)
        static const std::unordered_set<std::string> css_colors = {
            "white","black","transparent","red","green","blue","yellow","purple",
            "orange","pink","brown","cyan","magenta","lime","navy","teal","silver",
            "gray","grey","darkgray","lightgray","darkgrey","lightgrey",
            "whitesmoke","gainsboro","ghostwhite","aliceblue","azure","aqua",
            "aquamarine","turquoise","coral","salmon","tomato","goldenrod","gold",
            "khaki","wheat","tan","burlywood","sienna","maroon","darkred",
            "firebrick","crimson","orangered","darkorange","lavender","indigo",
            "violet","orchid","fuchsia","royalblue","dodgerblue","deepskyblue",
            "cornflowerblue","steelblue","lightblue","skyblue","cadetblue",
            "darkgreen","forestgreen","limegreen","yellowgreen","chartreuse",
            "lightgreen","springgreen","seagreen","darkslategray","slategray",
            "lightslategray","dimgray","dimgrey","currentcolor","inherit","none"
        };
        for (const auto& tok : p) {
            if (tok.empty()) continue;
            if (tok[0] == '#' || tok.rfind("rgb", 0) == 0 || tok.rfind("rgba", 0) == 0) {
                sv["background-color"] = tok;
                break;
            }
            if (css_colors.count(tok)) {
                sv["background-color"] = tok;
                break;
            }
        }
    }

    // flex shorthand: flex: <grow> [shrink] [basis]
    // e.g. flex:1 -> flex-grow:1, flex:1 1 0% -> flex-grow:1 flex-shrink:1 flex-basis:0%
    // Only expand if longhands not already set by a more specific rule
    if (sv.count("flex")) {
        const std::string& fv = sv.at("flex");
        if (fv != "none" && fv != "auto") {
            auto p = split_vals(fv);
            if (!p.empty() && !sv.count("flex-grow"))
                sv["flex-grow"] = p[0];
            if (p.size() >= 2 && !sv.count("flex-shrink"))
                sv["flex-shrink"] = p[1];
            if (p.size() >= 3 && !sv.count("flex-basis"))
                sv["flex-basis"] = p[2];
        } else if (fv == "none") {
            if (!sv.count("flex-grow"))   sv["flex-grow"]   = "0";
            if (!sv.count("flex-shrink")) sv["flex-shrink"] = "0";
            if (!sv.count("flex-basis"))  sv["flex-basis"]  = "auto";
        } else if (fv == "auto") {
            if (!sv.count("flex-grow"))   sv["flex-grow"]   = "1";
            if (!sv.count("flex-shrink")) sv["flex-shrink"] = "1";
            if (!sv.count("flex-basis"))  sv["flex-basis"]  = "auto";
        }
    }

    // border-radius shorthand: e.g. "24px" → border-radius: 24px
    // Already stored as-is; just normalize multi-value (use first value only)
    if (sv.count("border-radius") && !sv.at("border-radius").empty()) {
        auto p = split_vals(sv["border-radius"]);
        if (!p.empty()) sv["border-radius"] = p[0]; // use uniform radius
    }

    // border-top/right/bottom/left shorthands expand to border-color etc.
    // We just store them as-is for paint.cpp to handle.

    // Logical CSS properties → physical equivalents (LTR assumed)
    // padding-inline → padding-left / padding-right
    if (sv.count("padding-inline")) {
        auto p = split_vals(sv["padding-inline"]);
        if (!sv.count("padding-left"))  sv["padding-left"]  = p[0];
        if (!sv.count("padding-right")) sv["padding-right"] = p.size() >= 2 ? p[1] : p[0];
    }
    if (sv.count("padding-inline-start") && !sv.count("padding-left"))
        sv["padding-left"] = sv["padding-inline-start"];
    if (sv.count("padding-inline-end") && !sv.count("padding-right"))
        sv["padding-right"] = sv["padding-inline-end"];
    // padding-block → padding-top / padding-bottom
    if (sv.count("padding-block")) {
        auto p = split_vals(sv["padding-block"]);
        if (!sv.count("padding-top"))    sv["padding-top"]    = p[0];
        if (!sv.count("padding-bottom")) sv["padding-bottom"] = p.size() >= 2 ? p[1] : p[0];
    }
    if (sv.count("padding-block-start") && !sv.count("padding-top"))
        sv["padding-top"] = sv["padding-block-start"];
    if (sv.count("padding-block-end") && !sv.count("padding-bottom"))
        sv["padding-bottom"] = sv["padding-block-end"];
    // margin-inline → margin-left / margin-right
    if (sv.count("margin-inline")) {
        auto p = split_vals(sv["margin-inline"]);
        if (!sv.count("margin-left"))  sv["margin-left"]  = p[0];
        if (!sv.count("margin-right")) sv["margin-right"] = p.size() >= 2 ? p[1] : p[0];
    }
    if (sv.count("margin-inline-start") && !sv.count("margin-left"))
        sv["margin-left"] = sv["margin-inline-start"];
    if (sv.count("margin-inline-end") && !sv.count("margin-right"))
        sv["margin-right"] = sv["margin-inline-end"];
    // margin-block → margin-top / margin-bottom
    if (sv.count("margin-block")) {
        auto p = split_vals(sv["margin-block"]);
        if (!sv.count("margin-top"))    sv["margin-top"]    = p[0];
        if (!sv.count("margin-bottom")) sv["margin-bottom"] = p.size() >= 2 ? p[1] : p[0];
    }
    if (sv.count("margin-block-start") && !sv.count("margin-top"))
        sv["margin-top"] = sv["margin-block-start"];
    if (sv.count("margin-block-end") && !sv.count("margin-bottom"))
        sv["margin-bottom"] = sv["margin-block-end"];
    // inset-inline → left / right, inset-block → top / bottom
    if (sv.count("inset-inline")) {
        auto p = split_vals(sv["inset-inline"]);
        if (!sv.count("left"))  sv["left"]  = p[0];
        if (!sv.count("right")) sv["right"] = p.size() >= 2 ? p[1] : p[0];
    }
    if (sv.count("inset-block")) {
        auto p = split_vals(sv["inset-block"]);
        if (!sv.count("top"))    sv["top"]    = p[0];
        if (!sv.count("bottom")) sv["bottom"] = p.size() >= 2 ? p[1] : p[0];
    }
    // Note: `inset` shorthand is now expanded to top/right/bottom/left at
    // parse time by expand_shorthand() — no post-cascade handling needed.

    // block-size / inline-size → height / width
    if (sv.count("block-size") && !sv.count("height"))
        sv["height"] = sv["block-size"];
    if (sv.count("inline-size") && !sv.count("width"))
        sv["width"] = sv["inline-size"];
    if (sv.count("min-block-size") && !sv.count("min-height"))
        sv["min-height"] = sv["min-block-size"];
    if (sv.count("max-block-size") && !sv.count("max-height"))
        sv["max-height"] = sv["max-block-size"];
    if (sv.count("min-inline-size") && !sv.count("min-width"))
        sv["min-width"] = sv["min-inline-size"];
    if (sv.count("max-inline-size") && !sv.count("max-width"))
        sv["max-width"] = sv["max-inline-size"];
    // border-inline / border-block shorthands
    if (sv.count("border-inline-width")) {
        std::string v = sv["border-inline-width"];
        if (!sv.count("border-left-width"))  sv["border-left-width"]  = v;
        if (!sv.count("border-right-width")) sv["border-right-width"] = v;
    }
    if (sv.count("border-block-width")) {
        std::string v = sv["border-block-width"];
        if (!sv.count("border-top-width"))    sv["border-top-width"]    = v;
        if (!sv.count("border-bottom-width")) sv["border-bottom-width"] = v;
    }

    // transition/animation — ignore, we don't animate
    sv.erase("transition");
    sv.erase("animation");
    sv.erase("will-change");

    // Note: visibility:hidden intentionally NOT mapped to display:none
    // because Google's search bar is visibility:hidden by default and
    // shown via JS — if we hide it, the search bar disappears entirely.

    // overflow:hidden on zero-height containers — don't hide content, just ignore
    // (we don't clip, so this is a no-op)
  }

  // Inject ::before pseudo-element (only for Element nodes)
  if (root && root->type == NodeType::Element) {
    PropertyMap before_styles;
    std::string before_content = match_pseudo_element(root, stylesheet, index, "before", before_styles);
    if (!before_content.empty()) {
      // Create synthetic element wrapper with ::before styles
      auto pe_elem = std::make_shared<Node>(NodeType::Element, "span");
      pe_elem->attributes["__pseudo"] = "before";
      auto pe_text = std::make_shared<Node>(NodeType::Text, before_content);
      pe_elem->children.push_back(pe_text);

      auto pe_snode = std::make_shared<StyledNode>(pe_elem);
      pe_snode->parent = node;
      pe_snode->specified_values = before_styles;
      pe_snode->specified_values.erase("content"); // content is consumed

      auto pe_text_snode = std::make_shared<StyledNode>(pe_text);
      pe_text_snode->parent = pe_snode;
      pe_snode->children.push_back(pe_text_snode);

      node->children.push_back(pe_snode);
    }
  }

  for (auto &child : root->children)
    node->children.push_back(style_tree(child, stylesheet, index, node,
                                         depth + 1,
                                         hover_sheet, hover_index,
                                         focus_sheet, focus_index));

  // Inject ::after pseudo-element (only for Element nodes)
  if (root && root->type == NodeType::Element) {
    PropertyMap after_styles;
    std::string after_content = match_pseudo_element(root, stylesheet, index, "after", after_styles);
    if (!after_content.empty()) {
      auto pe_elem = std::make_shared<Node>(NodeType::Element, "span");
      pe_elem->attributes["__pseudo"] = "after";
      auto pe_text = std::make_shared<Node>(NodeType::Text, after_content);
      pe_elem->children.push_back(pe_text);

      auto pe_snode = std::make_shared<StyledNode>(pe_elem);
      pe_snode->parent = node;
      pe_snode->specified_values = after_styles;
      pe_snode->specified_values.erase("content");

      auto pe_text_snode = std::make_shared<StyledNode>(pe_text);
      pe_text_snode->parent = pe_snode;
      pe_snode->children.push_back(pe_text_snode);

      node->children.push_back(pe_snode);
    }
  }

  return node;
}

std::shared_ptr<StyledNode> build_style_tree(NodePtr root,
                                             const Stylesheet &stylesheet,
                                             const Stylesheet *hover_sheet,
                                             const Stylesheet *focus_sheet) {
  RuleIndex index = build_rule_index(stylesheet);

  // Build optional hover/focus indexes
  RuleIndex hover_idx, focus_idx;
  if (hover_sheet) hover_idx = build_rule_index(*hover_sheet);
  if (focus_sheet) focus_idx = build_rule_index(*focus_sheet);

  return style_tree(root, stylesheet, index, nullptr, 0,
                    hover_sheet  ? hover_sheet  : nullptr,
                    hover_sheet  ? &hover_idx   : nullptr,
                    focus_sheet  ? focus_sheet  : nullptr,
                    focus_sheet  ? &focus_idx   : nullptr);
}
