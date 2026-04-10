#pragma once
#include "dom.h"
#include "html_parser.h"
#include <stack>
#include <memory>

class DOMBuilder {
public:
  std::shared_ptr<Node> root;

  void push(std::shared_ptr<Node> node) {
    if (!node) return;

    if (node_count++ > HTMLParser::MAX_DOM_NODES)
        return;

    if (stack.empty()) {
        root = node;
        stack.push(node);
        return;
    }

    auto parent = stack.top();
    parent->append_child(node);

    if (node->type == NodeType::Element)
        stack.push(node);
  }

  void pop() {
      if (!stack.empty())
          stack.pop();
  }

private:
  std::stack<std::shared_ptr<Node>> stack;
  int node_count = 0;
};
