// selector.cpp — CSS selector matching and rule index
// Moved from style.cpp (Split D)
#include "selector.h"
#include <algorithm>
#include <sstream>
#include <unordered_map>

using NodePtr = std::shared_ptr<Node>;

/* =============================
   Specificity
   ============================= */

struct Specificity {
  int a = 0; // id
  int b = 0; // class, attribute, pseudo
  int c = 0; // tag

  bool operator>(const Specificity &o) const {
    if (a != o.a) return a > o.a;
    if (b != o.b) return b > o.b;
    return c > o.c;
  }
};

/* =============================
   Class matching — optimized
   ============================= */

// Fast class check: does space-separated `elem_classes` contain `cls`?
static inline bool has_class(const std::string &elem_classes, const std::string &cls) {
  size_t len = cls.size();
  size_t pos = 0;
  while ((pos = elem_classes.find(cls, pos)) != std::string::npos) {
    bool at_start = (pos == 0 || elem_classes[pos - 1] == ' ');
    bool at_end   = (pos + len == elem_classes.size() || elem_classes[pos + len] == ' ');
    if (at_start && at_end) return true;
    pos += len;
  }
  return false;
}

static bool class_match(const std::string &elem_classes,
                        const std::vector<std::string> &selector_classes) {
  for (const auto &cls : selector_classes) {
    if (!has_class(elem_classes, cls)) return false;
  }
  return true;
}

/* =============================
   An+B formula parsing
   ============================= */
struct AnB { int a; int b; };

// Parse An+B syntax: "odd", "even", "3", "2n+1", "-n+3", "n", "3n", etc.
static AnB parse_anb(const std::string &arg) {
  if (arg == "odd")  return {2, 1};
  if (arg == "even") return {2, 0};
  if (arg.empty())   return {0, 0};

  // Check if contains 'n'
  size_t n_pos = arg.find('n');
  if (n_pos == std::string::npos) {
    // Plain number
    try { return {0, std::stoi(arg)}; }
    catch (...) { return {0, 0}; }
  }

  // Has 'n' — parse A coefficient
  int a_val = 1;
  if (n_pos == 0) {
    a_val = 1;
  } else if (n_pos == 1 && arg[0] == '-') {
    a_val = -1;
  } else if (n_pos == 1 && arg[0] == '+') {
    a_val = 1;
  } else {
    try { a_val = std::stoi(arg.substr(0, n_pos)); }
    catch (...) { a_val = 1; }
  }

  // Parse B offset
  int b_val = 0;
  if (n_pos + 1 < arg.size()) {
    // Skip optional whitespace around +/-
    std::string rest = arg.substr(n_pos + 1);
    // Remove spaces
    rest.erase(std::remove(rest.begin(), rest.end(), ' '), rest.end());
    if (!rest.empty()) {
      try { b_val = std::stoi(rest); }
      catch (...) { b_val = 0; }
    }
  }
  return {a_val, b_val};
}

// Check if `index` (1-based) matches An+B
static inline bool matches_anb(const AnB &anb, int index) {
  if (anb.a == 0) return index == anb.b;
  int diff = index - anb.b;
  if (anb.a > 0) return diff >= 0 && diff % anb.a == 0;
  // a < 0: index <= b and (index - b) divisible by |a|
  return diff <= 0 && (-diff) % (-anb.a) == 0;
}

/* =============================
   Child index helpers (1-based, element-only)
   ============================= */

// Returns 1-based index among element siblings, -1 if not found
static int child_index(const NodePtr &node) {
  auto parent = node->parent.lock();
  if (!parent) return -1;
  int idx = 0;
  for (const auto &c : parent->children) {
    if (c && c->type == NodeType::Element) {
      ++idx;
      if (c.get() == node.get()) return idx;
    }
  }
  return -1;
}

// Returns 1-based index from end among element siblings
static int child_index_last(const NodePtr &node) {
  auto parent = node->parent.lock();
  if (!parent) return -1;
  int idx = 0;
  for (auto it = parent->children.rbegin(); it != parent->children.rend(); ++it) {
    if (*it && (*it)->type == NodeType::Element) {
      ++idx;
      if (it->get() == node.get()) return idx;
    }
  }
  return -1;
}

// Count of element siblings
static int element_child_count(const NodePtr &parent) {
  int count = 0;
  for (const auto &c : parent->children)
    if (c && c->type == NodeType::Element) ++count;
  return count;
}

// 1-based index among same-type element siblings
static int type_index(const NodePtr &node) {
  auto parent = node->parent.lock();
  if (!parent) return -1;
  const std::string &tag = node->data;
  int idx = 0;
  for (const auto &c : parent->children) {
    if (c && c->type == NodeType::Element && c->data == tag) {
      ++idx;
      if (c.get() == node.get()) return idx;
    }
  }
  return -1;
}

// 1-based index from end among same-type element siblings
static int type_index_last(const NodePtr &node) {
  auto parent = node->parent.lock();
  if (!parent) return -1;
  const std::string &tag = node->data;
  int idx = 0;
  for (auto it = parent->children.rbegin(); it != parent->children.rend(); ++it) {
    if (*it && (*it)->type == NodeType::Element && (*it)->data == tag) {
      ++idx;
      if (it->get() == node.get()) return idx;
    }
  }
  return -1;
}

// Count of same-type element siblings
static int type_child_count(const NodePtr &parent, const std::string &tag) {
  int count = 0;
  for (const auto &c : parent->children)
    if (c && c->type == NodeType::Element && c->data == tag) ++count;
  return count;
}

/* =============================
   :not() / :is() / :where() inner selector parsing
   ============================= */

// Forward declare match_simple for recursive :not/:is/:where
static bool match_simple(const NodePtr &elem, const SimpleSelector &sel);

// Parse a simple inner selector string like "div", ".foo", "#bar", "[type=text]"
// Returns true if elem matches. Supports comma-separated list.
static bool match_inner_selector(const NodePtr &elem, const std::string &arg) {
  if (arg.empty()) return false;
  // Split on comma for selector lists
  size_t start = 0;
  while (start < arg.size()) {
    size_t comma = arg.find(',', start);
    if (comma == std::string::npos) comma = arg.size();
    std::string part = arg.substr(start, comma - start);
    // Trim whitespace
    size_t fs = part.find_first_not_of(" \t");
    size_t fe = part.find_last_not_of(" \t");
    if (fs != std::string::npos) part = part.substr(fs, fe - fs + 1);
    else { start = comma + 1; continue; }

    // Build a SimpleSelector from the part
    SimpleSelector inner;
    size_t i = 0;
    while (i < part.size()) {
      if (part[i] == '.') {
        ++i;
        size_t s = i;
        while (i < part.size() && (isalnum((unsigned char)part[i]) || part[i] == '-' || part[i] == '_')) ++i;
        inner.classes.push_back(part.substr(s, i - s));
      } else if (part[i] == '#') {
        ++i;
        size_t s = i;
        while (i < part.size() && (isalnum((unsigned char)part[i]) || part[i] == '-' || part[i] == '_')) ++i;
        inner.id = part.substr(s, i - s);
      } else if (part[i] == '[') {
        // Simple attribute selector
        ++i;
        AttrSelector asel;
        size_t s = i;
        while (i < part.size() && part[i] != '=' && part[i] != ']' && part[i] != '^'
               && part[i] != '$' && part[i] != '*' && part[i] != '~' && part[i] != '|')
          ++i;
        asel.attr = part.substr(s, i - s);
        // Trim attr name
        while (!asel.attr.empty() && asel.attr.back() == ' ') asel.attr.pop_back();
        if (i < part.size() && part[i] != ']') {
          if (part[i] == '=') { asel.op = '='; ++i; }
          else { asel.op = part[i]; ++i; if (i < part.size() && part[i] == '=') ++i; }
          // Parse value
          if (i < part.size() && (part[i] == '"' || part[i] == '\'')) {
            char q = part[i++];
            size_t vs = i;
            while (i < part.size() && part[i] != q) ++i;
            asel.value = part.substr(vs, i - vs);
            if (i < part.size()) ++i;
          } else {
            size_t vs = i;
            while (i < part.size() && part[i] != ']') ++i;
            asel.value = part.substr(vs, i - vs);
          }
        }
        if (i < part.size() && part[i] == ']') ++i;
        inner.attrs.push_back(asel);
      } else if (part[i] == ':') {
        ++i;
        if (i < part.size() && part[i] == ':') ++i; // skip :: pseudo-elements in :not
        size_t s = i;
        while (i < part.size() && (isalnum((unsigned char)part[i]) || part[i] == '-')) ++i;
        std::string pname = part.substr(s, i - s);
        if (i < part.size() && part[i] == '(') {
          ++i;
          int depth = 1;
          size_t ps = i;
          while (i < part.size() && depth > 0) {
            if (part[i] == '(') depth++;
            else if (part[i] == ')') { depth--; if (depth == 0) break; }
            ++i;
          }
          pname += "(" + part.substr(ps, i - ps) + ")";
          if (i < part.size()) ++i;
        }
        inner.pseudo_classes.push_back(pname);
      } else if (part[i] == '*') {
        ++i; // universal - matches anything
      } else {
        size_t s = i;
        while (i < part.size() && (isalnum((unsigned char)part[i]) || part[i] == '-' || part[i] == '_')) ++i;
        if (i > s) inner.tag_name = part.substr(s, i - s);
        else ++i; // skip unknown char
      }
    }
    if (match_simple(elem, inner)) return true;
    start = comma + 1;
  }
  return false;
}

/* =============================
   Simple selector match
   ============================= */

static bool match_simple(const NodePtr &elem, const SimpleSelector &sel) {
  if (!elem || elem->type != NodeType::Element)
    return false;

  // Tag name
  if (!sel.tag_name.empty()) {
    if (lower(elem->data) != lower(sel.tag_name))
      return false;
  }

  // ID
  if (!sel.id.empty()) {
    auto it = elem->attributes.find("id");
    if (it == elem->attributes.end() || it->second != sel.id)
      return false;
  }

  // Classes
  if (!sel.classes.empty()) {
    auto it = elem->attributes.find("class");
    if (it == elem->attributes.end() || !class_match(it->second, sel.classes))
      return false;
  }

  // Attribute selectors: [attr], [attr=val], [attr^=val], [attr$=val], [attr*=val], [attr~=val], [attr|=val]
  for (const auto &asel : sel.attrs) {
    auto it = elem->attributes.find(asel.attr);
    if (it == elem->attributes.end()) return false;
    if (asel.op == 0) continue; // presence check only
    const std::string &actual = it->second;
    const std::string &expected = asel.value;
    bool ok = false;
    switch (asel.op) {
      case '=': ok = (actual == expected); break;
      case '^': ok = (actual.size() >= expected.size() &&
                      actual.compare(0, expected.size(), expected) == 0); break;
      case '$': ok = (actual.size() >= expected.size() &&
                      actual.compare(actual.size() - expected.size(), expected.size(), expected) == 0); break;
      case '*': ok = (actual.find(expected) != std::string::npos); break;
      case '~': {
        // Word match in space-separated list
        size_t len = expected.size(), pos = 0;
        while ((pos = actual.find(expected, pos)) != std::string::npos) {
          bool ws = (pos == 0 || actual[pos-1] == ' ');
          bool we = (pos+len == actual.size() || actual[pos+len] == ' ');
          if (ws && we) { ok = true; break; }
          pos += len;
        }
      } break;
      case '|': {
        // Prefix match: exact or followed by '-'
        ok = (actual == expected) ||
             (actual.size() > expected.size() &&
              actual.compare(0, expected.size(), expected) == 0 &&
              actual[expected.size()] == '-');
      } break;
      default: ok = (actual == expected); break;
    }
    if (!ok) return false;
  }

  // Pseudo-classes
  for (const auto &pc : sel.pseudo_classes) {
    // ── :root ──
    if (pc == "root") {
      auto par = elem->parent.lock();
      // Root is element with no element parent (or parent is document)
      if (par && par->type == NodeType::Element) return false;
      continue;
    }
    // ── :empty ──
    if (pc == "empty") {
      bool empty = true;
      for (const auto &c : elem->children) {
        if (!c) continue;
        if (c->type == NodeType::Element) { empty = false; break; }
        if (c->type == NodeType::Text && !c->data.empty()) { empty = false; break; }
      }
      if (!empty) return false;
      continue;
    }
    // ── :first-child ──
    if (pc == "first-child") {
      if (child_index(elem) != 1) return false;
      continue;
    }
    // ── :last-child ──
    if (pc == "last-child") {
      if (child_index_last(elem) != 1) return false;
      continue;
    }
    // ── :only-child ──
    if (pc == "only-child") {
      auto par = elem->parent.lock();
      if (!par || element_child_count(par) != 1) return false;
      continue;
    }
    // ── :first-of-type ──
    if (pc == "first-of-type") {
      if (type_index(elem) != 1) return false;
      continue;
    }
    // ── :last-of-type ──
    if (pc == "last-of-type") {
      if (type_index_last(elem) != 1) return false;
      continue;
    }
    // ── :only-of-type ──
    if (pc == "only-of-type") {
      auto par = elem->parent.lock();
      if (!par || type_child_count(par, elem->data) != 1) return false;
      continue;
    }
    // ── :nth-child(An+B) ──
    if (pc.compare(0, 10, "nth-child(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(10, pc.size() - 11);
      AnB anb = parse_anb(arg);
      int idx = child_index(elem);
      if (idx < 0 || !matches_anb(anb, idx)) return false;
      continue;
    }
    // ── :nth-last-child(An+B) ──
    if (pc.compare(0, 15, "nth-last-child(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(15, pc.size() - 16);
      AnB anb = parse_anb(arg);
      int idx = child_index_last(elem);
      if (idx < 0 || !matches_anb(anb, idx)) return false;
      continue;
    }
    // ── :nth-of-type(An+B) ──
    if (pc.compare(0, 13, "nth-of-type(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(12, pc.size() - 13);
      AnB anb = parse_anb(arg);
      int idx = type_index(elem);
      if (idx < 0 || !matches_anb(anb, idx)) return false;
      continue;
    }
    // ── :nth-last-of-type(An+B) ──
    if (pc.compare(0, 18, "nth-last-of-type(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(17, pc.size() - 18);
      AnB anb = parse_anb(arg);
      int idx = type_index_last(elem);
      if (idx < 0 || !matches_anb(anb, idx)) return false;
      continue;
    }
    // ── :not(selector) ──
    if (pc.compare(0, 4, "not(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(4, pc.size() - 5);
      if (match_inner_selector(elem, arg)) return false; // :not inverts
      continue;
    }
    // ── :is(selector) ──
    if (pc.compare(0, 3, "is(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(3, pc.size() - 4);
      if (!match_inner_selector(elem, arg)) return false;
      continue;
    }
    // ── :where(selector) — same as :is but zero specificity ──
    if (pc.compare(0, 6, "where(") == 0 && pc.back() == ')') {
      std::string arg = pc.substr(6, pc.size() - 7);
      if (!match_inner_selector(elem, arg)) return false;
      continue;
    }
    // ── :link / :any-link — unvisited links ──
    if (pc == "link" || pc == "any-link") {
      std::string tag = lower(elem->data);
      if (tag != "a" && tag != "area") return false;
      if (elem->attributes.find("href") == elem->attributes.end()) return false;
      continue;
    }
    // ── :checked ──
    if (pc == "checked") {
      if (elem->attributes.find("checked") == elem->attributes.end() &&
          elem->attributes.find("selected") == elem->attributes.end())
        return false;
      continue;
    }
    // ── :disabled ──
    if (pc == "disabled") {
      if (elem->attributes.find("disabled") == elem->attributes.end()) return false;
      continue;
    }
    // ── :enabled ──
    if (pc == "enabled") {
      std::string tag = lower(elem->data);
      bool is_form = (tag == "input" || tag == "button" || tag == "select" || tag == "textarea");
      if (!is_form || elem->attributes.find("disabled") != elem->attributes.end()) return false;
      continue;
    }
    // ── :required ──
    if (pc == "required") {
      if (elem->attributes.find("required") == elem->attributes.end()) return false;
      continue;
    }
    // ── :optional ──
    if (pc == "optional") {
      std::string tag = lower(elem->data);
      bool is_form = (tag == "input" || tag == "select" || tag == "textarea");
      if (!is_form || elem->attributes.find("required") != elem->attributes.end()) return false;
      continue;
    }
    // ── :read-only ──
    if (pc == "read-only") {
      std::string tag = lower(elem->data);
      bool is_input = (tag == "input" || tag == "textarea");
      if (is_input && elem->attributes.find("readonly") == elem->attributes.end()
          && elem->attributes.find("disabled") == elem->attributes.end()) return false;
      continue;
    }
    // ── :read-write ──
    if (pc == "read-write") {
      std::string tag = lower(elem->data);
      if (tag != "input" && tag != "textarea") return false;
      if (elem->attributes.find("readonly") != elem->attributes.end()) return false;
      if (elem->attributes.find("disabled") != elem->attributes.end()) return false;
      continue;
    }
    // ── :placeholder-shown ──
    if (pc == "placeholder-shown") {
      if (elem->attributes.find("placeholder") == elem->attributes.end()) return false;
      // Check if value is empty (has placeholder visible)
      auto val_it = elem->attributes.find("value");
      if (val_it != elem->attributes.end() && !val_it->second.empty()) return false;
      continue;
    }
    // ── :lang(xx) ──
    if (pc.compare(0, 5, "lang(") == 0 && pc.back() == ')') {
      std::string lang_arg = pc.substr(5, pc.size() - 6);
      // Walk up to find lang attribute
      bool found = false;
      auto cur = elem;
      while (cur) {
        auto it = cur->attributes.find("lang");
        if (it != cur->attributes.end()) {
          std::string el_lang = lower(it->second);
          std::string want = lower(lang_arg);
          found = (el_lang == want || (el_lang.size() > want.size() &&
                   el_lang.compare(0, want.size(), want) == 0 && el_lang[want.size()] == '-'));
          break;
        }
        cur = cur->parent.lock();
      }
      if (!found) return false;
      continue;
    }
    // ── :target ──
    if (pc == "target") {
      // We don't track URL fragments, so this never matches
      return false;
    }
    // ── :scope ──  (matches root in stylesheet context)
    if (pc == "scope") {
      auto par = elem->parent.lock();
      if (par && par->type == NodeType::Element) return false;
      continue;
    }
    // All other pseudo-classes are silently ignored (don't fail the match)
  }

  return true;
}

/* =============================
   Rule index
   ============================= */

RuleIndex build_rule_index(const Stylesheet &sheet) {

  RuleIndex index;

  for (const auto &rule : sheet.rules) {

    for (const auto &sel : rule.selectors) {
      if (sel.components.empty()) continue;
      const auto& last = sel.components.back();

      if (!last.id.empty()) {
        index.id_rules[last.id].push_back(&rule);
      } else if (!last.classes.empty()) {
        for (auto &c : last.classes)
          index.class_rules[c].push_back(&rule);
      } else if (!last.tag_name.empty()) {
        index.tag_rules[lower(last.tag_name)].push_back(&rule);
      } else {
        index.universal_rules.push_back(&rule);
      }
    }
  }

  return index;
}

void gather_rules(NodePtr elem, const RuleIndex &index,
                  std::vector<const Rule *> &out) {
  if (!elem || elem->type != NodeType::Element)
    return;

  auto id_it = elem->attributes.find("id");

  if (id_it != elem->attributes.end()) {

    auto it = index.id_rules.find(id_it->second);

    if (it != index.id_rules.end())
      out.insert(out.end(), it->second.begin(), it->second.end());
  }

  auto class_it = elem->attributes.find("class");

  if (class_it != elem->attributes.end()) {

    std::stringstream ss(class_it->second);

    std::string c;

    while (ss >> c) {

      auto it = index.class_rules.find(c);

      if (it != index.class_rules.end())
        out.insert(out.end(), it->second.begin(), it->second.end());
    }
  }

  auto tag_it = index.tag_rules.find(lower(elem->data));

  if (tag_it != index.tag_rules.end())
    out.insert(out.end(), tag_it->second.begin(), tag_it->second.end());

  out.insert(out.end(), index.universal_rules.begin(),
             index.universal_rules.end());
}

/* =============================
   Rule match
   ============================= */

static bool match_selector(NodePtr elem, const Selector &sel) {
  if (sel.components.empty()) return false;

  auto curr = elem;
  for (int i = (int)sel.components.size() - 1; i >= 0; --i) {
    const auto& comp = sel.components[i];

    if (i == (int)sel.components.size() - 1) {
      if (!match_simple(curr, comp)) return false;
    } else {
      // The relationship to the NEXT component (i+1) is stored in that component
      Combinator comb = sel.components[i + 1].combinator;

      if (comb == Combinator::Child) {
        curr = curr->parent.lock();
        if (!curr || !match_simple(curr, comp)) return false;
      } else if (comb == Combinator::Descendant) {
        bool found = false;
        curr = curr->parent.lock();
        while (curr) {
          if (match_simple(curr, comp)) {
            found = true;
            break;
          }
          curr = curr->parent.lock();
        }
        if (!found) return false;
      } else if (comb == Combinator::Adjacent) {
        curr = curr->prev_sibling.lock();
        if (!curr || !match_simple(curr, comp)) return false;
      } else if (comb == Combinator::Sibling) {
        bool found = false;
        curr = curr->prev_sibling.lock();
        while (curr) {
          if (match_simple(curr, comp)) {
            found = true;
            break;
          }
          curr = curr->prev_sibling.lock();
        }
        if (!found) return false;
      } else {
          // Should not happen with valid selectors
          if (!match_simple(curr, comp)) return false;
      }
    }
  }
  return true;
}

bool matches_rule(NodePtr elem, const Rule &rule) {

  for (const auto &sel : rule.selectors) {
    if (match_selector(elem, sel))
      return true;
  }

  return false;
}

/* =============================
   Rule application
   ============================= */

PropertyMap match_rules(NodePtr elem, const Stylesheet &sheet,
                        const RuleIndex &index) {
  PropertyMap styles;

  std::vector<const Rule *> candidates;
  gather_rules(elem, index, candidates);

  // Sort by specificity and order (implicit in candidates list order)
  struct MatchedRule {
    const Rule* rule;
    int specificity;
  };
  std::vector<MatchedRule> matched;

  for (auto rule : candidates) {
    int max_spec = -1;
    bool any_match = false;
    for (const auto& sel : rule->selectors) {
      // Skip pseudo-element selectors (::before/::after) in regular element matching.
      // These are handled exclusively by match_pseudo_element().
      if (!sel.components.empty() && !sel.components.back().pseudo_element.empty())
        continue;
      if (match_selector(elem, sel)) {
        any_match = true;
        max_spec = std::max(max_spec, sel.specificity());
      }
    }
    if (any_match) {
      matched.push_back({rule, max_spec});
    }
  }

  // Stable sort to maintain source order for same specificity
  std::stable_sort(matched.begin(), matched.end(), [](const MatchedRule& a, const MatchedRule& b) {
    return a.specificity < b.specificity;
  });

  // Apply non-important declarations first (sorted by specificity, last wins)
  for (auto& m : matched) {
    for (auto &decl : m.rule->declarations) {
      if (decl.important) continue;
      std::string val = decl.value;
      size_t vs = val.find_first_not_of(" \t\r\n");
      size_t ve = val.find_last_not_of(" \t\r\n");
      if (vs != std::string::npos) val = val.substr(vs, ve - vs + 1);
      styles[decl.name] = val;
    }
  }
  // Apply !important declarations after (override non-important regardless of specificity)
  for (auto& m : matched) {
    for (auto &decl : m.rule->declarations) {
      if (!decl.important) continue;
      std::string val = decl.value;
      size_t vs = val.find_first_not_of(" \t\r\n");
      size_t ve = val.find_last_not_of(" \t\r\n");
      if (vs != std::string::npos) val = val.substr(vs, ve - vs + 1);
      styles[decl.name] = val;
    }
  }

  return styles;
}

// Collect CSS declarations for ::before or ::after pseudo-elements
// Returns the content string (or "" if no matching rule) and sets out_styles
std::string match_pseudo_element(NodePtr elem, const Stylesheet &sheet,
                                 const RuleIndex &index,
                                 const std::string &pe, // "before" or "after"
                                 PropertyMap &out_styles) {
  if (!elem || elem->type != NodeType::Element) return "";

  std::vector<const Rule *> candidates;
  gather_rules(elem, index, candidates);

  for (auto rule : candidates) {
    for (const auto &sel : rule->selectors) {
      if (sel.components.empty()) continue;
      const auto &last = sel.components.back();
      if (last.pseudo_element != pe) continue;
      // Match the base selector (without pseudo-element)
      Selector base_sel;
      base_sel.components = sel.components;
      base_sel.components.back().pseudo_element = ""; // strip pseudo
      if (!match_selector(elem, base_sel)) continue;
      for (auto &decl : rule->declarations) {
        std::string val = decl.value;
        size_t vs = val.find_first_not_of(" \t\r\n");
        size_t ve = val.find_last_not_of(" \t\r\n");
        if (vs != std::string::npos) val = val.substr(vs, ve - vs + 1);
        out_styles[decl.name] = val;
      }
    }
  }

  auto it = out_styles.find("content");
  if (it == out_styles.end()) return "";

  std::string content_val = it->second;
  // Strip quotes from "text" or 'text'
  if (content_val.size() >= 2 &&
      ((content_val.front() == '"' && content_val.back() == '"') ||
       (content_val.front() == '\'' && content_val.back() == '\''))) {
    content_val = content_val.substr(1, content_val.size() - 2);
  }
  // Skip display:none, content:none, content:normal
  if (content_val == "none" || content_val == "normal") return "";
  return content_val;
}
