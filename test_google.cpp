#include "src/css_parser.h"
#include "src/html_parser.h"
#include "src/js.h"
#include "src/layout.h"
#include "src/net.h"
#include "src/paint.h"
#include "src/style.h"
#include <fstream>
#include <iostream>
#include <string>

int main() {
  std::cout << "Fetching google.com..." << std::endl;
  std::string html = fetch_https("https://google.com");
  if (html.empty()) {
    std::cout << "Fetch failed." << std::endl;
    return 1;
  }

  std::cout << "Parsing HTML..." << std::endl;
  HTMLParser parser(html);
  auto root = parser.parse();
  if (!root) {
    std::cout << "HTML Parsing failed." << std::endl;
    return 1;
  }

  int ast_nodes = 0;
  auto count_ast = [](auto &self, auto &box, int &count) -> void {
    count++;
    if (box) {
      for (auto &c : box->children)
        self(self, c, count);
    }
  };
  count_ast(count_ast, root, ast_nodes);
  std::cout << "AST nodes: " << ast_nodes << std::endl;

  std::cout << "Running JS..." << std::endl;
  auto run_scripts = [](auto &self, std::shared_ptr<Node> node) -> void {
    if (!node)
      return;
    if (node->type == NodeType::Element && node->data == "script") {
      std::string source;
      for (auto &child : node->children) {
        if (child->type == NodeType::Text) {
          source += child->data;
        }
      }
      try {
        js::run_js(source);
      } catch (...) {
      }
    } else {
      for (auto &child : node->children) {
        self(self, child);
      }
    }
  };
  run_scripts(run_scripts, root);

  std::cout << "Parsing CSS..." << std::endl;
  std::string css =
      "head, script, style, meta, link, title { display: none; }\n";

  auto extract_styles = [](auto &self, std::shared_ptr<Node> node,
                           std::string &out_css) -> void {
    if (!node)
      return;
    if (node->type == NodeType::Element && node->data == "style") {
      for (auto &child : node->children) {
        if (child->type == NodeType::Text) {
          out_css += child->data + "\n";
        }
      }
    } else {
      for (auto &child : node->children) {
        self(self, child, out_css);
      }
    }
  };
  // extract_styles(extract_styles, root, css);

  CSSParser css_parser(css);
  auto stylesheet = css_parser.parse_stylesheet();

  std::cout << "Building Style Tree..." << std::endl;
  auto styleTreeRoot = style_tree(root, stylesheet);
  if (styleTreeRoot) {
    print_style_tree(styleTreeRoot, 0);
  }

  std::cout << "Building Layout Tree..." << std::endl;
  auto layoutRoot = build_layout_tree(styleTreeRoot);

  std::cout << "Performing Layout..." << std::endl;
  if (layoutRoot) {
    int nodes = 0;
    auto count_nodes = [](auto &self, auto &box, int &count) -> void {
      count++;
      if (box) {
        for (auto &c : box->children)
          self(self, c, count);
      }
    };
    count_nodes(count_nodes, layoutRoot, nodes);
    std::cout << "Layout nodes: " << nodes << std::endl;
    Dimensions viewport;
    viewport.content.width = 800;
    viewport.content.height = 600;
    layoutRoot->layout(viewport);

    print_layout_tree(layoutRoot, 0);

    std::cout << "Building Display List..." << std::endl;
    auto list = build_display_list(layoutRoot);
    std::cout << "Display list items: " << list.size() << std::endl;
  } else {
    std::cout << "Layout root is null!" << std::endl;
  }

  std::cout << "Success!" << std::endl;
  return 0;
}
