#pragma once
#include <string>
#include <vector>


enum class Combinator { Descendant, Child, Adjacent, Sibling, None };

// Attribute selector: [attr], [attr=val], [attr^=val], [attr$=val], [attr*=val], [attr~=val]
struct AttrSelector {
  std::string attr;   // attribute name
  std::string value;  // expected value (empty = presence-only)
  char op = 0;        // 0=present, '='=equals, '^'=starts, '$'=ends, '*'=contains, '~'=word, '|'=prefix
};

struct SimpleSelector {
  std::string tag_name;
  std::string id;
  std::vector<std::string> classes;
  std::vector<AttrSelector> attrs;           // [attr=val] selectors
  std::vector<std::string> pseudo_classes;   // :first-child, :last-child, :nth-child(n), :not()
  std::string pseudo_element;               // "before" or "after" (from ::before / ::after)
  Combinator combinator = Combinator::None;
};

struct Selector {
  std::vector<SimpleSelector> components;
  
  // Specificity calc
  int specificity() const {
    int a = 0, b = 0, c = 0;
    for (const auto& s : components) {
        if (!s.id.empty()) a++;
        b += (int)s.classes.size();
        b += (int)s.attrs.size();          // attribute selectors = class specificity
        b += (int)s.pseudo_classes.size(); // pseudo-classes = class specificity
        if (!s.tag_name.empty()) c++;
    }
    return (a << 16) | (b << 8) | c;
  }
};

struct Declaration {
  std::string name;
  std::string value;
  bool important = false; // true if !important was present
};

struct Rule {
  std::vector<Selector> selectors;
  std::vector<Declaration> declarations;
};

struct Stylesheet {
  std::vector<Rule> rules;
};

