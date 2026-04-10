#include "html_parser.h"
#include "dom_builder.h"
#include <cassert>

HTMLParser::HTMLParser(const std::string &input) : input(input), pos(0) {}

int HTMLParser::node_counter = 0;

bool HTMLParser::eof() const { return pos >= input.length(); }

bool HTMLParser::starts_with(const std::string &s) const {
  if (pos + s.length() > input.length())
    return false;
  return input.substr(pos, s.length()) == s;
}

char HTMLParser::next_char() const {
  if (eof())
    return '\0';
  return input[pos];
}

char HTMLParser::consume_char() {
  if (eof())
    return '\0';
  return input[pos++];
}

std::string HTMLParser::consume_while(bool (*test)(char)) {
  std::string result;
  while (!eof() && test(next_char())) {
    result += consume_char();
  }
  return result;
}

void HTMLParser::consume_whitespace() {
  consume_while(
      [](char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; });
}

std::string HTMLParser::parse_tag_name() {
  return consume_while([](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
  });
}

std::shared_ptr<Node> HTMLParser::parse_node() {
  std::shared_ptr<Node> result;
  parse_node_stream([&](std::shared_ptr<Node> n) {
    if (!result && n)
      result = n;
  });
  return result;
}

std::string decode_html_entities(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] == '&') {
      size_t end = text.find(';', i);
      if (end != std::string::npos && end - i < 10) {
        std::string ent = text.substr(i + 1, end - i - 1);
        if (ent.length() > 0 && ent[0] == '#') {
          int code = 0;
          try {
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
              code = std::stoi(ent.substr(2), nullptr, 16);
            } else {
              code = std::stoi(ent.substr(1));
            }
          } catch (...) {
          }
          if (code > 0 && code < 0x10FFFF) {
            if (code <= 0x7F) {
              out += (char)code;
            } else if (code <= 0x7FF) {
              out += (char)(0xC0 | ((code >> 6) & 0x1F));
              out += (char)(0x80 | (code & 0x3F));
            } else if (code <= 0xFFFF) {
              out += (char)(0xE0 | ((code >> 12) & 0x0F));
              out += (char)(0x80 | ((code >> 6) & 0x3F));
              out += (char)(0x80 | (code & 0x3F));
            } else {
              out += (char)(0xF0 | ((code >> 18) & 0x07));
              out += (char)(0x80 | ((code >> 12) & 0x3F));
              out += (char)(0x80 | ((code >> 6) & 0x3F));
              out += (char)(0x80 | (code & 0x3F));
            }
          }
          i = end;
          continue;
        } else if (ent == "raquo") {
          out += "\xC2\xBB";
          i = end;
          continue;
        } else if (ent == "nbsp") {
          out += " ";
          i = end;
          continue;
        } else if (ent == "amp") {
          out += "&";
          i = end;
          continue;
        } else if (ent == "lt") {
          out += "<";
          i = end;
          continue;
        } else if (ent == "gt") {
          out += ">";
          i = end;
          continue;
        } else if (ent == "copy") {
          out += "\xC2\xA9";
          i = end;
          continue;
        } else if (ent == "quot") {
          out += "\"";
          i = end;
          continue;
        } else if (ent == "apos") {
          out += "'";
          i = end;
          continue;
        }
      }
    }
    out += text[i];
  }
  return out;
}

std::shared_ptr<Node> HTMLParser::parse_text() {
  std::string raw = consume_while([](char c) { return c != '<'; });

  // Collapse whitespace
  std::string collapsed;
  bool in_space = false;
  for (char c : raw) {
    if (std::isspace((unsigned char)c)) {
      if (!in_space) {
        collapsed += ' ';
        in_space = true;
      }
    } else {
      collapsed += c;
      in_space = false;
    }
  }

  return TextNode(decode_html_entities(collapsed));
}

std::shared_ptr<Node> HTMLParser::parse_element() {
  DOMBuilder builder;
  parse_element_stream([&](std::shared_ptr<Node> n) {
    if (!n)
      builder.pop();
    else
      builder.push(n);
  });
  return builder.root;
}

std::string HTMLParser::parse_attr_value() {
  char open_quote = consume_char();
  if (open_quote != '"' && open_quote != '\'') {
    // Unquoted attribute
    std::string value = "";
    value += open_quote;
    value +=
        consume_while([](char c) { return c != ' ' && c != '>' && c != '/'; });
    return value;
  }

  std::string value;
  while (!eof() && next_char() != open_quote) {
    value += consume_char();
  }
  if (!eof())
    consume_char(); // consume closing quote
  return value;
}

std::pair<std::string, std::string> HTMLParser::parse_attr() {
  std::string name = parse_tag_name();
  std::string value;
  if (!eof() && next_char() == '=') {
    consume_char(); // consume '='
    value = parse_attr_value();
  }
  return {name, value};
}

AttrMap HTMLParser::parse_attributes() {
  AttrMap attributes;
  while (!eof()) {
    consume_whitespace();
    if (next_char() == '>' || next_char() == '/')
      break;
    size_t start_pos = pos;
    auto attr = parse_attr();
    if (!attr.first.empty()) {
      // Lowercase attribute name
      for (char &c : attr.first) {
        if (c >= 'A' && c <= 'Z')
          c = c - 'A' + 'a';
      }
      attributes[attr.first] = attr.second;
    }
    if (pos == start_pos && !eof()) {
      consume_char(); // Prevent infinite loop on unrecognized characters
    }
  }
  return attributes;
}

std::vector<std::shared_ptr<Node>> HTMLParser::parse_nodes() {
  std::vector<std::shared_ptr<Node>> nodes;

  while (!eof()) {
    consume_whitespace();
    if (eof())
      break;
    // Stop if we hit a closing tag — we are inside a parent element
    if (starts_with("</"))
      break;

    DOMBuilder builder;
    size_t before = pos;
    parse_node_stream([&](std::shared_ptr<Node> n) {
      if (!n)
        builder.pop();
      else
        builder.push(n);
    });

    if (builder.root)
      nodes.push_back(builder.root);

    // Guard against infinite loop if nothing was consumed
    if (pos == before && !eof()) {
      consume_char();
    }
  }

  return nodes;
}

std::vector<std::shared_ptr<Node>> HTMLParser::parse_fragment() {
  return parse_nodes();
}

std::shared_ptr<Node> HTMLParser::parse() {
  DOMBuilder builder;
  parse_stream([&](std::shared_ptr<Node> node) {
    if (!node)
      builder.pop();
    else
      builder.push(node);
  });

  if (!builder.root)
    return nullptr;
  return builder.root;
}
