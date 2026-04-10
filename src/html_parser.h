#pragma once
#include "dom.h"
#include <string>

class HTMLParser {
public:
  HTMLParser(const std::string &input);
  std::shared_ptr<Node> parse();
  std::vector<std::shared_ptr<Node>> parse_fragment();

  template <typename Callback> void parse_stream(Callback on_node) {
    while (!eof()) {
      parse_node_stream(on_node);
    }
  }

  static const int MAX_DOM_NODES = 15000;
  static const int MAX_DEPTH = 60;
  static int node_counter;

private:
  std::string input;
  size_t pos;
  int depth = 0;

  bool eof() const;
  bool starts_with(const std::string &s) const;
  char next_char() const;
  char consume_char();
  std::string consume_while(bool (*test)(char));
  void consume_whitespace();

  std::string parse_tag_name();
  std::shared_ptr<Node> parse_text();
  std::pair<std::string, std::string> parse_attr();
  std::string parse_attr_value();
  AttrMap parse_attributes();

  // Internal streaming helpers
  template <typename Callback> void parse_node_stream(Callback on_node) {
    if (++node_counter > MAX_DOM_NODES) {
      on_node(TextNode(""));
      return;
    }
    if (depth > MAX_DEPTH) {
      on_node(TextNode(""));
      return;
    }

    if (next_char() == '<') {
      parse_element_stream(on_node);
    } else {
      on_node(parse_text());
    }
  }

  template <typename Callback> void parse_element_stream(Callback on_node) {
    size_t saved_pos = pos;
    consume_char(); // <

    if (starts_with("!--")) {
      pos += 3;
      while (!eof() && !starts_with("-->"))
        pos++;
      if (!eof())
        pos += 3;
      on_node(TextNode(""));
      return;
    }
    if (next_char() == '!' || next_char() == '?' || starts_with("![")) {
      while (!eof() && next_char() != '>')
        consume_char();
      if (!eof())
        consume_char();
      on_node(TextNode(""));
      return;
    }

    std::string tag_name = parse_tag_name();
    if (tag_name.empty()) {
      pos = saved_pos;
      std::string text;
      while (!eof()) {
        char c = consume_char();
        text += c;
        if (c == '>')
          break;
      }
      on_node(TextNode(text));
      return;
    }

    for (char &c : tag_name) {
      if (c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';
    }

    AttrMap attrs = parse_attributes();
    bool self_closing = false;
    if (!eof() && next_char() == '/') {
      consume_char();
      self_closing = true;
    }
    if (!eof() && next_char() == '>') {
      consume_char();
    }

    auto node = ElementNode(tag_name, attrs);
    on_node(node);

    bool is_void =
        (self_closing || tag_name == "meta" || tag_name == "link" ||
         tag_name == "img" || tag_name == "br" || tag_name == "hr" ||
         tag_name == "input" || tag_name == "base" || tag_name == "col" ||
         tag_name == "embed" || tag_name == "source" || tag_name == "track" ||
         tag_name == "wbr" || tag_name == "area" || tag_name == "param");

    if (!is_void) {
      if (tag_name == "script" || tag_name == "style") {
        while (!eof() && !starts_with("</" + tag_name)) {
          consume_char();
        }
      } else {
        depth++;
        parse_nodes_stream(on_node);
        depth--;
      }

      // Consume closing tag
      if (!eof() && starts_with("</")) {
        size_t startpos = pos;
        consume_char(); // <
        consume_char(); // /
        std::string closing_tag = parse_tag_name();
        for (char &c : closing_tag) {
          if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
        }
        consume_whitespace();
        if (!eof() && next_char() == '>') {
          consume_char();
        }
        if (closing_tag != tag_name) {
          pos = startpos;
        }
      }
    }

    on_node(nullptr); // signal close
  }

  template <typename Callback> void parse_nodes_stream(Callback on_node) {
    while (!eof() && !starts_with("</")) {
      size_t before = pos;
      parse_node_stream(on_node);
      if (pos == before) {
        consume_char();
      }
    }
  }

  // Deprecated - kept for compatibility if needed, but we'll reimplement
  std::shared_ptr<Node> parse_node();
  std::shared_ptr<Node> parse_element();
  std::vector<std::shared_ptr<Node>> parse_nodes();
};
