#include "css_parser.h"
#include <cstddef>
#include <sstream>
#include <stdexcept>

// Split a CSS shorthand value into 1-4 space-separated tokens.
// Tracks paren depth so values like `rgb(1,2,3)` aren't broken apart.
static std::vector<std::string> split_shorthand_tokens(const std::string &v) {
  std::vector<std::string> parts;
  std::string cur;
  int depth = 0;
  for (char c : v) {
    if (c == '(') { depth++; cur += c; }
    else if (c == ')') { depth--; cur += c; }
    else if ((c == ' ' || c == '\t') && depth == 0) {
      if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

// Expand a 4-sided box shorthand value (1-4 tokens) into top/right/bottom/left.
// Returns four strings in TRBL order.
static void expand_4sided(const std::string &value, std::string &top,
                          std::string &right, std::string &bottom,
                          std::string &left) {
  auto parts = split_shorthand_tokens(value);
  if (parts.empty()) return;
  if (parts.size() == 1) {
    top = right = bottom = left = parts[0];
  } else if (parts.size() == 2) {
    top = bottom = parts[0];
    left = right = parts[1];
  } else if (parts.size() == 3) {
    top = parts[0];
    left = right = parts[1];
    bottom = parts[2];
  } else {
    top    = parts[0];
    right  = parts[1];
    bottom = parts[2];
    left   = parts[3];
  }
}

std::vector<Declaration> expand_shorthand(const Declaration &decl) {
  std::vector<Declaration> out;

  auto push = [&](const char *name, const std::string &val) {
    if (val.empty()) return;
    Declaration d;
    d.name = name;
    d.value = val;
    d.important = decl.important;
    out.push_back(std::move(d));
  };

  if (decl.name == "margin") {
    std::string t, r, b, l;
    expand_4sided(decl.value, t, r, b, l);
    push("margin-top", t);
    push("margin-right", r);
    push("margin-bottom", b);
    push("margin-left", l);
    return out;
  }
  if (decl.name == "padding") {
    std::string t, r, b, l;
    expand_4sided(decl.value, t, r, b, l);
    push("padding-top", t);
    push("padding-right", r);
    push("padding-bottom", b);
    push("padding-left", l);
    return out;
  }
  if (decl.name == "inset") {
    std::string t, r, b, l;
    expand_4sided(decl.value, t, r, b, l);
    push("top", t);
    push("right", r);
    push("bottom", b);
    push("left", l);
    return out;
  }

  // Not a recognized shorthand — pass through unchanged.
  out.push_back(decl);
  return out;
}


CSSParser::CSSParser(const std::string &input) : input(input), pos(0) {

  static const size_t MAX_CSS_SIZE = 10 * 1024 * 1024; // 10MB safety limit
  if (input.size() > MAX_CSS_SIZE)
    throw std::runtime_error("CSS input too large");
}

bool CSSParser::eof() const { return pos >= input.length(); }

inline char CSSParser::next_char() const {
  return pos < input.length() ? input[pos] : '\0';
}

char CSSParser::consume_char() {
  return pos < input.length() ? input[pos++] : '\0';
}

const char space_c = ' ';
const char backsn_c = '\n';
const char backst = '\t';
const char backsr = '\r';
const char smalla_c = 'a';
const char smallz_c = 'z';
const char capa_c = 'A';
const char capz_c = 'Z';
const char zero_c = '0';
const char nine_c = '9';
const char hiphen_c = '-';
const char underscore_c = '_';
const char openbrace_c = '{';
const char closebrace_c = '}';

static const size_t MAX_IDENTIFIER = 256;
static const size_t MAX_VALUE_LENGTH = 8192;  // increased for complex gradient values
static const size_t MAX_SELECTORS = 64;
static const size_t MAX_CLASSES = 64;
static const int MAX_DECLS = 200;
static const int MAX_RULES = 100000;  // was 2000 — Google CSS has 5000+ rules

std::string CSSParser::consume_while(bool (*test)(char)) {

  size_t start = pos;

  while (!eof() && test(next_char()))
    pos++;

  return input.substr(start, pos - start);
}

void CSSParser::consume_whitespace() {

  while (!eof()) {

    char c = next_char();

    if (c == space_c || c == backsn_c || c == backst || c == backsr)
      pos++;
    else
      break;
  }
}

std::string CSSParser::parse_identifier() {

  size_t start = pos;

  while (!eof()) {

    char c = next_char();

    bool valid = (c >= smalla_c && c <= smallz_c) ||
                 (c >= capa_c && c <= capz_c) || (c >= zero_c && c <= nine_c) ||
                 c == hiphen_c || c == underscore_c;

    if (!valid)
      break;

    if (pos - start >= MAX_IDENTIFIER)
      break;

    pos++;
  }

  return input.substr(start, pos - start);
}

SimpleSelector CSSParser::parse_simple_selector() {

  SimpleSelector selector;

  while (!eof()) {

    char c = next_char();
    if (c == '*') {
      consume_char(); // universal selector — consume it so the parser doesn't stall
      break;
    }
    if (c == ' ' || c == ',' || c == openbrace_c || c == '>' || c == '+' || c == '~' || c == '(' || c == ')') {
      break;
    }

    if (c == '#') {
      consume_char();
      selector.id = parse_identifier();
    } else if (c == '.') {
      consume_char();
      if (selector.classes.size() < MAX_CLASSES)
        selector.classes.push_back(parse_identifier());
      else
        parse_identifier(); // discard

    } else if (c == '[') {
      // Attribute selector: [attr], [attr=val], [attr^=val], [attr$=val], [attr*=val]
      consume_char(); // '['
      consume_whitespace();
      AttrSelector asel;
      asel.attr = parse_identifier();
      consume_whitespace();
      if (!eof() && next_char() != ']') {
        char op2 = consume_char();
        if (op2 == '=' ) {
          asel.op = '=';
        } else if (!eof() && next_char() == '=') {
          asel.op = op2; // ^= $= *= ~= |=
          consume_char();
        }
        consume_whitespace();
        // parse value (quoted or unquoted)
        if (!eof() && (next_char() == '"' || next_char() == '\'')) {
          char q = consume_char();
          while (!eof() && next_char() != q) asel.value += consume_char();
          if (!eof()) consume_char(); // closing quote
        } else {
          while (!eof() && next_char() != ']') asel.value += consume_char();
        }
      }
      consume_whitespace();
      if (!eof() && next_char() == ']') consume_char(); // ']'
      if (!asel.attr.empty())
        selector.attrs.push_back(asel);

    } else if (c == ':') {
      consume_char(); // first ':'
      bool is_pseudo_elem = (!eof() && next_char() == ':');
      if (is_pseudo_elem) consume_char(); // second ':'
      std::string pname = parse_identifier();
      // Handle function-form :pseudo(...)
      std::string parg;
      if (!eof() && next_char() == '(') {
        consume_char(); // '('
        int depth = 1;
        while (!eof() && depth > 0) {
          char fc = consume_char();
          if (fc == '(') { depth++; parg += fc; }
          else if (fc == ')') { depth--; if (depth > 0) parg += fc; }
          else parg += fc;
        }
      }
      // Store ::before / ::after pseudo-elements; skip other pseudo-elements
      if (is_pseudo_elem) {
        if (pname == "before" || pname == "after") {
          selector.pseudo_element = pname;
        }
        // other pseudo-elements (::placeholder, ::selection, etc.) are ignored
      } else {
        // Skip state-based pseudo-classes (handled via hover/focus sheets elsewhere)
        bool skip = (pname == "hover" || pname == "focus" || pname == "active" ||
                     pname == "visited" || pname == "focus-within" || pname == "focus-visible" ||
                     pname == "placeholder");
        if (!skip) {
          std::string full = pname;
          if (!parg.empty()) full += "(" + parg + ")";
          selector.pseudo_classes.push_back(full);
        }
      }

    } else {
      size_t before = pos;
      selector.tag_name = parse_identifier();
      if (pos == before) {
        // Unknown character that can't start an identifier (e.g. &, |, !, %)
        // — consume it and stop so the loop doesn't spin forever
        consume_char();
        break;
      }
    }
  }

  return selector;
}

std::vector<Selector> CSSParser::parse_selectors() {

  std::vector<Selector> selectors;
  selectors.reserve(8);

  while (!eof() && selectors.size() < MAX_SELECTORS) {

    consume_whitespace();

    if (next_char() == openbrace_c)
      break;

    Selector sel;
    while (!eof()) {
        consume_whitespace();
        char next = next_char();
        if (next == ',' || next == openbrace_c || next == '\0') break;
        
        Combinator comb = Combinator::Descendant;
        if (next == '>') { consume_char(); comb = Combinator::Child; }
        else if (next == '+') { consume_char(); comb = Combinator::Adjacent; }
        else if (next == '~') { consume_char(); comb = Combinator::Sibling; }
        
        consume_whitespace();
        size_t pos_before = pos;
        auto simple = parse_simple_selector();
        if (pos == pos_before && !eof()) {
          if (next_char() == '(') {
            // Skip entire CSS function argument list e.g. :not(...), :nth-child(...)
            int depth = 1;
            consume_char(); // consume '('
            while (!eof() && depth > 0) {
              char fc = consume_char();
              if (fc == '(') depth++;
              else if (fc == ')') depth--;
            }
            // Also discard the function name that was already pushed as a bare tag
            if (!sel.components.empty() && sel.components.back().id.empty()
                && sel.components.back().classes.empty())
              sel.components.pop_back();
          } else {
            consume_char(); // skip any other stuck character
          }
          continue;
        }
        simple.combinator = comb;
        if (sel.components.empty()) simple.combinator = Combinator::None;
        sel.components.push_back(simple);
    }

    if (!sel.components.empty())
        selectors.push_back(sel);

    consume_whitespace();

    if (next_char() == ',')
      consume_char();
  }

  return selectors;
}

Declaration CSSParser::parse_declaration() {

  Declaration decl;

  consume_whitespace();

  decl.name = parse_identifier();

  consume_whitespace();

  if (next_char() == ':')
    consume_char();

  consume_whitespace();

  size_t start = pos;

  while (!eof()) {

    char c = next_char();

    if (c == ';' || c == closebrace_c)
      break;

    if (pos - start >= MAX_VALUE_LENGTH)
      break;

    pos++;
  }

  decl.value = input.substr(start, pos - start);

  // Detect and strip !important
  {
    size_t imp = decl.value.rfind("!important");
    if (imp == std::string::npos) {
      std::string lower_v = decl.value;
      for (char &c : lower_v) c = (char)tolower((unsigned char)c);
      imp = lower_v.rfind("!important");
    }
    if (imp != std::string::npos) {
      decl.important = true;
      decl.value.erase(imp);
    }
  }

  size_t end = decl.value.find_last_not_of(" \n\r\t");
  if (end != std::string::npos)
    decl.value.erase(end + 1);

  if (!eof() && next_char() == ';')
    consume_char();

  return decl;
}

std::vector<Declaration> CSSParser::parse_declarations() {

  std::vector<Declaration> declarations;
  declarations.reserve(16);

  if (consume_char() != openbrace_c)
    return declarations;

  int count = 0;

  while (!eof() && count < MAX_DECLS) {

    consume_whitespace();

    if (next_char() == closebrace_c) {
      consume_char();
      break;
    }

    if (next_char() == openbrace_c) {

      int depth = 1;
      consume_char();

      while (!eof() && depth > 0) {

        char c = consume_char();

        if (c == openbrace_c)
          depth++;
        else if (c == closebrace_c)
          depth--;
      }

      continue;
    }

    size_t before = pos;

    Declaration d = parse_declaration();

    if (!d.name.empty()) {
      // Expand box-model shorthands (margin, padding, inset) at parse time
      // so the cascade only sees longhands. Non-shorthand declarations come
      // back unchanged as a single-element vector.
      auto expanded = expand_shorthand(d);
      for (auto &ed : expanded)
        declarations.push_back(std::move(ed));
    }

    count++;

    if (pos == before && !eof())
      consume_char();
  }

  if (count >= MAX_DECLS) {

    int depth = 1;

    while (!eof() && depth > 0) {

      char c = consume_char();

      if (c == openbrace_c)
        depth++;
      else if (c == closebrace_c)
        depth--;
    }
  }

  return declarations;
}

Rule CSSParser::parse_rule() {

  Rule rule;

  rule.selectors = parse_selectors();
  rule.declarations = parse_declarations();

  return rule;
}

Stylesheet CSSParser::parse_stylesheet() {

  try {

    Stylesheet stylesheet;
    stylesheet.rules.reserve(128);

    int rule_count = 0;

    while (!eof() && rule_count < MAX_RULES) {

      consume_whitespace();

      if (eof())
        break;

      if (pos + 1 < input.length() && input[pos] == '/' &&
          input[pos + 1] == '*') {

        pos += 2;

        while (!eof()) {

          if (pos + 1 < input.length() && input[pos] == '*' &&
              input[pos + 1] == '/') {

            pos += 2;
            break;
          }

          pos++;
        }

        continue;
      }

      if (next_char() == '@') {
        size_t at_start = pos;
        int depth = 0;
        bool in_block = false;

        while (!eof()) {

          char c = consume_char();

          if (c == openbrace_c) {
            depth++;
            in_block = true;
          }

          else if (c == closebrace_c) {

            depth--;

            if (depth <= 0)
              break;
          }

          else if (!in_block && c == ';') {
            break;
          }
        }

        continue;
      }

      if (next_char() == closebrace_c) {

        consume_char();
        continue;
      }

      size_t before = pos;

      Rule r = parse_rule();
      stylesheet.rules.push_back(r);

      rule_count++;

      if (pos == before && !eof())
        consume_char();
    }

    return stylesheet;
  }

  catch (const std::bad_alloc &) {

    throw std::runtime_error("Memory allocation failed while parsing CSS");
  }
}