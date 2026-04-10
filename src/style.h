#pragma once
#include "css.h"
#include "dom.h"
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Map of CSS Property Names to their Computed Values
using PropertyMap = std::map<std::string, std::string>;

class StyledNode {
public:
  std::shared_ptr<Node> node;   // Pointer to the original DOM node
  PropertyMap specified_values; // CSS properties calculated for this node
  std::vector<std::shared_ptr<StyledNode>> children;
  std::weak_ptr<StyledNode> parent;

  StyledNode(std::shared_ptr<Node> n) : node(n) {}

  // Helper function to query a computed property with inheritance
  std::string value(const std::string &name) const {
    auto it = specified_values.find(name);
    if (it != specified_values.end()) {
      // Resolve explicit "inherit" keyword — use parent's computed value
      if (it->second == "inherit") {
        if (auto p = parent.lock()) return p->value(name);
        return "";
      }
      return it->second;
    }

    // Properties that inherit by default (static set for O(1) lookup)
    static const std::unordered_set<std::string> inherited = {
        "color",         "font-family",  "font-size",   "font-weight",
        "font-style",    "line-height",  "text-align",  "visibility",
        "white-space",   "text-transform", "letter-spacing",
        "word-spacing",  "cursor",        "list-style-type",
        "overflow-wrap", "word-break",    "font-variant"};

    bool should_inherit = inherited.count(name) > 0;

    if (should_inherit) {
      if (auto p = parent.lock()) {
        return p->value(name);
      }
    }

    return "";
  }
};

// Build a tree of StyledNodes representing the combined DOM and CSS
std::shared_ptr<StyledNode>
build_style_tree(std::shared_ptr<Node> root, const Stylesheet &stylesheet,
                 const Stylesheet *hover_stylesheet = nullptr,
                 const Stylesheet *focus_stylesheet = nullptr);


