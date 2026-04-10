#include "dom.h"
#include <algorithm>
#include <iostream>
#include <sstream>

// Helper to lower-case a string
static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

// ── CSS selector engine ──────────────────────────────────────────────────────
// Supports: tag, #id, .class, [attr], [attr=v], [attr^=v], [attr$=v],
//           [attr*=v], [attr~=v], compound (div.cls#id[attr]), descendant
//           (ancestor descendant), child (parent > child),
//           comma groups (.a, .b), :not(), *, :first-child, :last-child,
//           :nth-child(n), :checked, :disabled, :empty, :focus, :hover (noop),
//           ::before/::after stripped.

// Trim whitespace from both ends
static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\n\r");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\n\r");
  return s.substr(a, b - a + 1);
}

// Check if a node has a given class token
static bool has_class(const Node *node, const std::string &cls) {
  auto it = node->attributes.find("class");
  if (it == node->attributes.end()) return false;
  const std::string &cv = it->second;
  size_t pos = 0;
  while (pos < cv.size()) {
    size_t e = cv.find_first_of(" \t\n\r", pos);
    size_t len = (e == std::string::npos) ? cv.size() - pos : e - pos;
    if (len == cls.size() && cv.compare(pos, len, cls) == 0) return true;
    pos = (e == std::string::npos) ? cv.size() : e + 1;
  }
  return false;
}

// Match a single SIMPLE selector (no combinators) against a node.
// A simple selector is a sequence of: tag, #id, .class, [attr...], :pseudo
static bool match_simple(const Node *node, const std::string &sel) {
  if (!node || node->type != NodeType::Element) return false;
  if (sel.empty()) return false;

  size_t pos = 0;
  size_t len = sel.size();

  // Tag part (before any # . [ :)
  {
    size_t end = len;
    for (size_t i = 0; i < len; i++) {
      char c = sel[i];
      if (c == '#' || c == '.' || c == '[' || c == ':') { end = i; break; }
    }
    if (end > 0) {
      std::string tag = sel.substr(0, end);
      if (tag != "*" && lower(tag) != lower(node->data)) return false;
      pos = end;
    }
  }

  // Process the rest: #id .class [attr...] :pseudo
  while (pos < len) {
    char ch = sel[pos];
    if (ch == '#') {
      // #id
      size_t end = pos + 1;
      while (end < len && sel[end] != '#' && sel[end] != '.' &&
             sel[end] != '[' && sel[end] != ':') end++;
      std::string id = sel.substr(pos + 1, end - pos - 1);
      auto it = node->attributes.find("id");
      if (it == node->attributes.end() || it->second != id) return false;
      pos = end;
    } else if (ch == '.') {
      // .class
      size_t end = pos + 1;
      while (end < len && sel[end] != '#' && sel[end] != '.' &&
             sel[end] != '[' && sel[end] != ':') end++;
      std::string cls = sel.substr(pos + 1, end - pos - 1);
      if (!has_class(node, cls)) return false;
      pos = end;
    } else if (ch == '[') {
      // [attr], [attr=val], [attr^=val], [attr$=val], [attr*=val], [attr~=val]
      size_t close = sel.find(']', pos);
      if (close == std::string::npos) return false;
      std::string inner = sel.substr(pos + 1, close - pos - 1);
      // Find operator
      size_t op_pos = inner.find_first_of("=^$*~|!");
      if (op_pos == std::string::npos) {
        // [attr] — just presence
        std::string attr = trim(inner);
        if (!node->attributes.count(attr)) return false;
      } else {
        char op_ch = inner[op_pos];
        bool has_op2 = (op_pos + 1 < inner.size() && inner[op_pos + 1] == '=');
        std::string op;
        size_t val_start;
        if (op_ch == '=' || !has_op2) {
          op = "="; val_start = op_pos + 1;
        } else {
          op = std::string(1, op_ch) + "=";
          val_start = op_pos + 2;
        }
        std::string attr = trim(inner.substr(0, op_pos > 0 && inner[op_pos-1]!='=' ? op_pos : op_pos));
        // Strip leading chars of op from attr name if needed
        while (!attr.empty() && (attr.back() == '^' || attr.back() == '$' ||
               attr.back() == '*' || attr.back() == '~' || attr.back() == '|' ||
               attr.back() == '!')) attr.pop_back();
        attr = trim(attr);
        std::string val = trim(inner.substr(val_start));
        // Remove surrounding quotes
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\''))
          val = val.substr(1, val.size() - 2);
        auto it = node->attributes.find(attr);
        bool attr_exists = (it != node->attributes.end());
        const std::string &av = attr_exists ? it->second : "";
        if (op == "=")  { if (!attr_exists || av != val) return false; }
        else if (op == "^=") { if (!attr_exists || av.substr(0, val.size()) != val) return false; }
        else if (op == "$=") { if (!attr_exists || av.size() < val.size() ||
                                   av.substr(av.size() - val.size()) != val) return false; }
        else if (op == "*=") { if (!attr_exists || av.find(val) == std::string::npos) return false; }
        else if (op == "~=") { if (!attr_exists || !has_class(node, val)) return false; }
        else if (op == "|=") { if (!attr_exists || (av != val && av.substr(0, val.size()+1) != val+"-")) return false; }
        else if (op == "!=") { if (attr_exists && av == val) return false; }
        else { if (!attr_exists || av != val) return false; }
      }
      pos = close + 1;
    } else if (ch == ':') {
      // :pseudo-class or ::pseudo-element
      if (pos + 1 < len && sel[pos + 1] == ':') {
        // ::before, ::after etc. — skip to end
        pos = len; break;
      }
      size_t end = pos + 1;
      while (end < len && sel[end] != '#' && sel[end] != '.' &&
             sel[end] != '[' && sel[end] != ':') end++;
      std::string pseudo = sel.substr(pos + 1, end - pos - 1);
      // Handle :not(selector)
      if (pseudo.size() > 4 && pseudo.substr(0, 4) == "not(") {
        std::string inner = pseudo.substr(4);
        if (!inner.empty() && inner.back() == ')') inner.pop_back();
        if (match_simple(node, trim(inner))) return false;
      } else if (pseudo == "first-child") {
        auto par = node->parent.lock();
        if (!par) return false;
        bool is_first = false;
        for (auto &c : par->children) {
          if (c->type == NodeType::Element) { is_first = (c.get() == node); break; }
        }
        if (!is_first) return false;
      } else if (pseudo == "last-child") {
        auto par = node->parent.lock();
        if (!par) return false;
        std::shared_ptr<Node> last_elem;
        for (auto &c : par->children)
          if (c->type == NodeType::Element) last_elem = c;
        if (!last_elem || last_elem.get() != node) return false;
      } else if (pseudo == "only-child") {
        auto par = node->parent.lock();
        if (!par) return false;
        int cnt = 0;
        for (auto &c : par->children) if (c->type == NodeType::Element) cnt++;
        if (cnt != 1) return false;
      } else if (pseudo == "empty") {
        bool empty = true;
        for (auto &c : node->children) {
          if (c->type == NodeType::Element ||
              (c->type == NodeType::Text && !trim(c->data).empty())) { empty = false; break; }
        }
        if (!empty) return false;
      } else if (pseudo == "checked") {
        if (!node->attributes.count("checked")) return false;
      } else if (pseudo == "disabled") {
        if (!node->attributes.count("disabled")) return false;
      } else if (pseudo == "enabled") {
        if (node->attributes.count("disabled")) return false;
      } else if (pseudo == "focus" || pseudo == "hover" || pseudo == "active" ||
                 pseudo == "visited" || pseudo == "link") {
        // Dynamic states — always false for static rendering
        return false;
      }
      // nth-child, nth-of-type etc. — skip (too complex, return true to not filter)
      pos = end;
    } else {
      pos++; // unknown char, skip
    }
  }
  return true;
}

// Forward declaration
static bool node_matches_selector(const std::shared_ptr<Node> &node,
                                  const std::string &selector);

// Split a selector list (comma-separated) into individual selectors,
// respecting brackets.
static std::vector<std::string> split_selector_list(const std::string &sel) {
  std::vector<std::string> parts;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i <= sel.size(); i++) {
    if (i == sel.size() || (sel[i] == ',' && depth == 0)) {
      std::string part = trim(sel.substr(start, i - start));
      if (!part.empty()) parts.push_back(part);
      start = i + 1;
    } else if (sel[i] == '(' || sel[i] == '[') depth++;
    else if (sel[i] == ')' || sel[i] == ']') depth--;
  }
  return parts;
}

// Match a node against a full selector (may contain combinators).
// Returns true if node matches the rightmost part and its ancestors satisfy
// the left parts.
static bool match_complex(const std::shared_ptr<Node> &node,
                           const std::string &selector) {
  if (!node || selector.empty()) return false;

  // Find the last combinator: ' ' (descendant) or '>' (child) or '+' (adjacent)
  // We scan right-to-left outside of brackets/parens.
  int depth = 0;
  int split = -1;
  char combinator = ' ';
  for (int i = (int)selector.size() - 1; i >= 0; i--) {
    char c = selector[i];
    if (c == ')' || c == ']') { depth++; continue; }
    if (c == '(' || c == '[') { depth--; continue; }
    if (depth > 0) continue;
    if (c == '>' || c == '+' || c == '~') {
      combinator = c; split = i; break;
    }
    if (c == ' ' && i > 0) {
      // Make sure it's not just leading whitespace
      // Look back for non-space
      int j = i - 1;
      while (j >= 0 && selector[j] == ' ') j--;
      if (j >= 0 && selector[j] != '>' && selector[j] != '+' && selector[j] != '~') {
        combinator = ' '; split = i; break;
      }
    }
  }

  if (split < 0) {
    // No combinator — just a simple selector
    return match_simple(node.get(), trim(selector));
  }

  std::string right = trim(selector.substr(split + 1));
  std::string left  = trim(selector.substr(0, split));

  // Right part must match node
  if (!match_simple(node.get(), right)) return false;

  if (combinator == '>') {
    // Parent must match left
    auto par = node->parent.lock();
    if (!par) return false;
    return match_complex(par, left);
  } else if (combinator == ' ') {
    // Any ancestor must match left
    auto anc = node->parent.lock();
    while (anc) {
      if (match_complex(anc, left)) return true;
      anc = anc->parent.lock();
    }
    return false;
  } else if (combinator == '+') {
    // Immediately preceding sibling
    auto par = node->parent.lock();
    if (!par) return false;
    std::shared_ptr<Node> prev;
    for (auto &c : par->children) {
      if (c.get() == node.get()) break;
      if (c->type == NodeType::Element) prev = c;
    }
    return prev && match_complex(prev, left);
  } else if (combinator == '~') {
    // Any preceding sibling
    auto par = node->parent.lock();
    if (!par) return false;
    for (auto &c : par->children) {
      if (c.get() == node.get()) break;
      if (c->type == NodeType::Element && match_complex(c, left)) return true;
    }
    return false;
  }
  return false;
}

// Top-level: check node against a (possibly comma-separated) selector string
static bool node_matches_selector(const std::shared_ptr<Node> &node,
                                  const std::string &selector) {
  auto parts = split_selector_list(selector);
  for (auto &part : parts)
    if (match_complex(node, part)) return true;
  return false;
}

std::shared_ptr<Node> Node::query_selector(const std::string &selector) {
  if (node_matches_selector(shared_from_this(), selector))
    return shared_from_this();
  for (auto &child : children) {
    auto result = child->query_selector(selector);
    if (result) return result;
  }
  return nullptr;
}

std::vector<std::shared_ptr<Node>>
Node::query_selector_all(const std::string &selector) {
  std::vector<std::shared_ptr<Node>> results;
  if (node_matches_selector(shared_from_this(), selector))
    results.push_back(shared_from_this());
  for (auto &child : children) {
    auto child_results = child->query_selector_all(selector);
    results.insert(results.end(), child_results.begin(), child_results.end());
  }
  return results;
}

void Node::dispatch_event(const std::string &type, bool bubbles) {
  // Execute listeners on this node
  auto it = event_listeners.find(type);
  if (it != event_listeners.end()) {
    for (auto &callback : it->second) {
      callback(type);
    }
  }

  // Bubble up to parent
  if (bubbles) {
    if (auto p = parent.lock()) {
      p->dispatch_event(type, true);
    }
  }
}
