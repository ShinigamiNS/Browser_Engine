#pragma once
#include "css.h"
#include <string>
#include <vector>

// Expand a CSS declaration into longhand declarations.
//
// For shorthands like `padding`, `margin`, `inset`, returns the four
// longhand declarations (padding-top, padding-right, padding-bottom,
// padding-left). For non-shorthand declarations, returns the input
// unchanged as a single-element vector.
//
// This runs at parse time (both for stylesheet rules and inline styles)
// so the cascade only ever has to deal with longhands. This mirrors how
// Servo's PropertyDeclarationBlock stores declarations.
std::vector<Declaration> expand_shorthand(const Declaration &decl);

class CSSParser {
public:
  CSSParser(const std::string &input);
  Stylesheet parse_stylesheet();

private:
  std::string input;
  size_t pos;

  bool eof() const;
  char next_char() const;
  char consume_char();
  std::string consume_while(bool (*test)(char));
  void consume_whitespace();

  Rule parse_rule();
  std::vector<Selector> parse_selectors();
  SimpleSelector parse_simple_selector();
  std::vector<Declaration> parse_declarations();
  Declaration parse_declaration();

  std::string parse_identifier();
};
