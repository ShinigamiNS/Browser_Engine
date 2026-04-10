#pragma once
// selector.h — CSS selector matching and rule index
// Moved from style.cpp (Split D)
#include "style.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ── Fast lowercase helpers (inline, ASCII only) ──
// Shared by selector.cpp and style.cpp for UA tag matching.
static inline void lower_inplace(std::string &s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z') c += 32;
}
static inline std::string lower(std::string s) { lower_inplace(s); return s; }

// ── Rule index for fast selector matching ──
struct RuleIndex {
  std::unordered_map<std::string, std::vector<const Rule *>> tag_rules;
  std::unordered_map<std::string, std::vector<const Rule *>> class_rules;
  std::unordered_map<std::string, std::vector<const Rule *>> id_rules;
  std::vector<const Rule *> universal_rules;
};

// Build an index over the stylesheet for fast per-element candidate lookup.
RuleIndex build_rule_index(const Stylesheet &sheet);

// Collect candidate rules for elem from the index.
void gather_rules(std::shared_ptr<Node> elem, const RuleIndex &index,
                  std::vector<const Rule *> &out);

// Return true if elem matches any selector in rule.
bool matches_rule(std::shared_ptr<Node> elem, const Rule &rule);

// Compute the merged PropertyMap for elem by applying all matching rules,
// sorted by specificity.
PropertyMap match_rules(std::shared_ptr<Node> elem, const Stylesheet &sheet,
                        const RuleIndex &index);

// Collect CSS declarations for ::before or ::after pseudo-elements.
// Returns the content string (or "" if none) and fills out_styles.
std::string match_pseudo_element(std::shared_ptr<Node> elem,
                                 const Stylesheet &sheet,
                                 const RuleIndex &index,
                                 const std::string &pe,
                                 PropertyMap &out_styles);
