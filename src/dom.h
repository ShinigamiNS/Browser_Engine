#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>


enum class NodeType { Text, Element };

using AttrMap = std::map<std::string, std::string>;

class Node : public std::enable_shared_from_this<Node> {
public:
  NodeType type;
  std::string data; // Used for text node content or element tag name
  AttrMap attributes;
  std::vector<std::shared_ptr<Node>> children;

  std::weak_ptr<Node> parent;
  std::shared_ptr<Node> next_sibling;
  std::weak_ptr<Node> prev_sibling;

  Node(NodeType t, const std::string &d) : type(t), data(d) {}

  void append_child(std::shared_ptr<Node> child) {
    if (!child) return;
    
    // Set parent
    child->parent = shared_from_this();
    
    // Set siblings
    if (!children.empty()) {
      auto last_child = children.back();
      last_child->next_sibling = child;
      child->prev_sibling = last_child;
    }
    
    children.push_back(child);
  }

  void remove_child(std::shared_ptr<Node> child) {
    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
      if (child->next_sibling) {
        child->next_sibling->prev_sibling = child->prev_sibling;
      }
      if (auto prev = child->prev_sibling.lock()) {
        prev->next_sibling = child->next_sibling;
      }
      child->parent.reset();
      child->next_sibling.reset();
      child->prev_sibling.reset();
      children.erase(it);
    }
  }

  void set_attribute(const std::string &name, const std::string &value) {
    attributes[name] = value;
  }

  // --- Query Selectors ---
  std::shared_ptr<Node> query_selector(const std::string &selector);
  std::vector<std::shared_ptr<Node>> query_selector_all(const std::string &selector);

  // --- Events ---
  using EventCallback = std::function<void(const std::string &type)>;
  void add_event_listener(const std::string &type, EventCallback callback) {
    event_listeners[type].push_back(callback);
  }
  void dispatch_event(const std::string &type, bool bubbles = true);

private:
  std::map<std::string, std::vector<EventCallback>> event_listeners;
};

inline std::shared_ptr<Node> ElementNode(const std::string &name,
                                         const AttrMap &attrs = {}) {
  auto node = std::make_shared<Node>(NodeType::Element, name);
  node->attributes = attrs;
  return node;
}

inline std::shared_ptr<Node> TextNode(const std::string &text) {
  return std::make_shared<Node>(NodeType::Text, text);
}

