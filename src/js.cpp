#include "js.h"
#include "dom.h"
#include "html_parser.h"
#include <cctype>
#include <iostream>


namespace js {

std::vector<Token> tokenize(const std::string &input) {
  std::vector<Token> tokens;
  size_t i = 0;
  while (i < input.length()) {
    char c = input[i];
    if (std::isspace(c)) {
      i++;
    } else if (std::isalpha(c) || c == '_') {
      std::string ident;
      while (i < input.length() &&
             (std::isalnum(input[i]) || input[i] == '_')) {
        ident += input[i++];
      }
      if (ident == "let" || ident == "var" || ident == "const" ||
          ident == "function") {
        tokens.push_back({TokenType::Keyword, ident});
      } else {
        tokens.push_back({TokenType::Identifier, ident});
      }
    } else if (std::isdigit(c)) {
      std::string num;
      while (i < input.length() &&
             (std::isdigit(input[i]) || input[i] == '.')) {
        num += input[i++];
      }
      tokens.push_back({TokenType::Number, num});
    } else if (c == '"' || c == '\'') {
      char quote = input[i++];
      std::string str;
      while (i < input.length() && input[i] != quote) {
        str += input[i++];
      }
      if (i < input.length())
        i++; // consume quote
      tokens.push_back({TokenType::String, str});
    } else if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/' ||
               c == '<' || c == '>') {
      tokens.push_back({TokenType::Operator, std::string(1, c)});
      i++;
    } else if (c == '(' || c == ')' || c == '{' || c == '}' || c == ';' ||
               c == '.' || c == ',') {
      tokens.push_back({TokenType::Punctuation, std::string(1, c)});
      i++;
    } else {
      i++; // skip unknown
    }
  }
  tokens.push_back({TokenType::Eof, ""});
  return tokens;
}

std::shared_ptr<Value> make_undefined() {
  auto v = std::make_shared<Value>();
  v->type = ValueType::Undefined;
  return v;
}
std::shared_ptr<Value> make_null() {
  auto v = std::make_shared<Value>();
  v->type = ValueType::Null;
  return v;
}
std::shared_ptr<Value> make_number(double n) {
  auto v = std::make_shared<Value>();
  v->type = ValueType::Number;
  v->num = n;
  return v;
}
std::shared_ptr<Value> make_string(const std::string &s) {
  auto v = std::make_shared<Value>();
  v->type = ValueType::String;
  v->str = s;
  return v;
}
std::shared_ptr<Value> make_function(NativeFunction f) {
  auto v = std::make_shared<Value>();
  v->type = ValueType::Function;
  v->native_func = f;
  return v;
}
std::shared_ptr<Value> make_object() {
  auto v = std::make_shared<Value>();
  v->type = ValueType::Object;
  return v;
}

std::shared_ptr<Value> wrap_style(std::shared_ptr<Node> node) {
  if (!node) return make_null();
  auto v = make_object();
  v->internal_ptr = node;

  auto styles = {"background", "background-color", "color", "width", "height", "display", "padding", "margin"};
  for (auto s : styles) {
    v->setters[s] = [node, s](std::shared_ptr<Value> val) {
      if (val->type == ValueType::String) {
        std::string current = node->attributes["style"];
        node->attributes["style"] = current + " " + s + ": " + val->str + ";";
      }
    };
  }
  return v;
}

std::shared_ptr<Value> wrap_node(std::shared_ptr<Node> node) {
  if (!node) return make_null();
  auto v = make_object();
  v->internal_ptr = node;
  
  v->properties["tagName"] = make_string(node->data);
  v->properties["style"] = wrap_style(node);
  
  v->getters["innerHTML"] = [node](const std::vector<std::shared_ptr<Value>>&) {
    return make_string("TODO: innerHTML getter");
  };

  v->setters["innerHTML"] = [node](std::shared_ptr<Value> val) {
    if (val->type == ValueType::String) {
      HTMLParser parser(val->str);
      auto new_children = parser.parse_fragment();
      node->children.clear();
      for (auto& child : new_children) {
        node->append_child(child);
      }
    }
  };
  v->properties["classList"] = make_object();
  v->properties["classList"]->properties["add"] = make_function([node](const std::vector<std::shared_ptr<Value>>& args) {
      if (!args.empty() && args[0]->type == ValueType::String) {
          std::string& cls = node->attributes["class"];
          if (!cls.empty()) cls += " ";
          cls += args[0]->str;
      }
      return make_undefined();
  });

  v->properties["appendChild"] = make_function([node](const std::vector<std::shared_ptr<Value>>& args) {
      if (!args.empty() && args[0]->type == ValueType::Object && args[0]->internal_ptr) {
          auto child = std::static_pointer_cast<Node>(args[0]->internal_ptr);
          node->append_child(child);
      }
      return args.empty() ? make_null() : args[0];
  });

  v->properties["removeChild"] = make_function([node](const std::vector<std::shared_ptr<Value>>& args) {
      if (!args.empty() && args[0]->type == ValueType::Object && args[0]->internal_ptr) {
          auto child = std::static_pointer_cast<Node>(args[0]->internal_ptr);
          node->remove_child(child);
      }
      return args.empty() ? make_null() : args[0];
  });

  return v;
}

std::shared_ptr<Value> Environment::get(const std::string &name) {
  if (vars.count(name))
    return vars[name];
  if (global_obj && global_obj->properties.count(name))
    return global_obj->properties[name];
  if (parent)
    return parent->get(name);
  return make_undefined();
}
void Environment::set(const std::string &name, std::shared_ptr<Value> val) {
  vars[name] = val;
}

struct NumberNode : public ASTNode {
  double num;
  NumberNode(double n) : num(n) {}
  std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) override {
    return make_number(num);
  }
};

struct StringNode : public ASTNode {
  std::string str;
  StringNode(const std::string &s) : str(s) {}
  std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) override {
    return make_string(str);
  }
};

struct IdentifierNode : public ASTNode {
  std::string name;
  IdentifierNode(const std::string &n) : name(n) {}
  std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) override {
    return env->get(name);
  }
};

struct MemberAccessNode : public ASTNode {
  std::shared_ptr<ASTNode> obj;
  std::string member;
  MemberAccessNode(std::shared_ptr<ASTNode> o, const std::string &m)
      : obj(o), member(m) {}
  std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) override {
    auto val = obj ? obj->eval(env) : make_undefined();
    if (val) {
      if (val->getters.count(member)) {
        return val->getters[member]({});
      }
      if (val->properties.count(member)) {
        return val->properties[member];
      }
    }
    return make_undefined();
  }
};

struct AssignmentNode : public ASTNode {
  std::shared_ptr<ASTNode> target;
  std::shared_ptr<ASTNode> value;
  AssignmentNode(std::shared_ptr<ASTNode> t, std::shared_ptr<ASTNode> v)
      : target(t), value(v) {}
  std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) override {
    auto val = value ? value->eval(env) : make_undefined();
    
    if (auto ident = std::dynamic_pointer_cast<IdentifierNode>(target)) {
      env->set(ident->name, val);
    } else if (auto member = std::dynamic_pointer_cast<MemberAccessNode>(target)) {
      auto obj_val = member->obj ? member->obj->eval(env) : nullptr;
      if (obj_val) {
        if (obj_val->setters.count(member->member)) {
          obj_val->setters[member->member](val);
        } else {
          obj_val->properties[member->member] = val;
        }
      }
    }
    return val;
  }
};


struct FunctionCallNode : public ASTNode {
  std::shared_ptr<ASTNode> func;
  std::vector<std::shared_ptr<ASTNode>> args;
  FunctionCallNode(std::shared_ptr<ASTNode> f,
                   const std::vector<std::shared_ptr<ASTNode>> &a)
      : func(f), args(a) {}
  std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) override {
    auto fval = func ? func->eval(env) : make_undefined();
    if (fval->type == ValueType::Function && fval->native_func) {
      std::vector<std::shared_ptr<Value>> eval_args;
      for (auto &a : args) {
        eval_args.push_back(a ? a->eval(env) : make_undefined());
      }
      return fval->native_func(eval_args);
    }
    return make_undefined();
  }
};

class Parser {
  std::vector<Token> tokens;
  size_t pos_ = 0;

  Token peek() {
    return pos_ < tokens.size() ? tokens[pos_] : Token{TokenType::Eof, ""};
  }
  Token consume() {
    return pos_ < tokens.size() ? tokens[pos_++] : Token{TokenType::Eof, ""};
  }

public:
  Parser(const std::vector<Token> &t) : tokens(t) {}

  std::shared_ptr<ASTNode> parse_expression() {
    Token t = peek();
    std::shared_ptr<ASTNode> left;

    if (t.type == TokenType::Number) {
      consume();
      left = std::make_shared<NumberNode>(std::stod(t.value));
    } else if (t.type == TokenType::String) {
      consume();
      left = std::make_shared<StringNode>(t.value);
    } else if (t.type == TokenType::Identifier) {
      consume();
      left = std::make_shared<IdentifierNode>(t.value);
    }

    // Handle postfix operators like . and (
    while (true) {
      Token next = peek();
      if (next.value == ".") {
        consume();
        Token member = consume();
        left = std::make_shared<MemberAccessNode>(left, member.value);
      } else if (next.value == "(") {
        consume();
        std::vector<std::shared_ptr<ASTNode>> args;
        if (peek().value != ")") {
          args.push_back(parse_expression());
          while (peek().value == ",") {
            consume();
            args.push_back(parse_expression());
          }
        }
        if (peek().value == ")") consume();
        left = std::make_shared<FunctionCallNode>(left, args);
      } else {
        break;
      }
    }

    return left;
  }

  std::shared_ptr<ASTNode> parse_statement() {
    if (peek().type == TokenType::Keyword &&
        (peek().value == "let" || peek().value == "var" ||
         peek().value == "const")) {
      consume(); // var/let
      Token ident = consume();
      std::string name = ident.value;
      if (peek().value == "=") {
        consume();
        auto expr = parse_expression();
        if (peek().value == ";")
          consume();
        return std::make_shared<AssignmentNode>(std::make_shared<IdentifierNode>(name), expr);
      }
    } else {
      auto expr = parse_expression();
      if (expr && peek().value == "=") {
        consume();
        auto rhs = parse_expression();
        if (peek().value == ";")
          consume();
        return std::make_shared<AssignmentNode>(expr, rhs);
      }
      if (peek().value == ";")
        consume();
      return expr;
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<ASTNode>> parse() {
    std::vector<std::shared_ptr<ASTNode>> nodes;
    while (peek().type != TokenType::Eof) {
      auto stmt = parse_statement();
      if (stmt) {
        nodes.push_back(stmt);
      } else {
        consume(); // Parse error fallback
      }
    }
    return nodes;
  }
};

std::vector<std::shared_ptr<ASTNode>> parse(const std::vector<Token> &tokens) {
  Parser p(tokens);
  return p.parse();
}

std::shared_ptr<Value> run_js(const std::string &source, std::shared_ptr<Environment> env) {
  auto tokens = tokenize(source);
  auto ast = parse(tokens);

  if (!env) {
    env = std::make_shared<Environment>();
  }
  
  if (!env->global_obj) {
    auto window = make_object();
    env->global_obj = window;
    window->properties["window"] = window;

    auto console = make_object();
    window->properties["console"] = console;
    
    // We need a way to pass the actual DOM root here. 
    // For now, we'll assume the environment might be pre-populated or we add a placeholder.
    auto document = make_object();
    window->properties["document"] = document;

    document->properties["querySelector"] = make_function([env](const std::vector<std::shared_ptr<Value>> &args) {
      if (args.empty() || args[0]->type != ValueType::String) return make_null();
      
      auto doc_val = env->global_obj->properties["document"];
      auto root = std::static_pointer_cast<Node>(doc_val->internal_ptr);
      
      if (root) {
        auto found = root->query_selector(args[0]->str);
        return wrap_node(found);
      }
      return make_null();
    });

    document->properties["createElement"] = make_function([](const std::vector<std::shared_ptr<Value>> &args) {
      if (args.empty() || args[0]->type != ValueType::String) return make_null();
      auto node = std::make_shared<Node>(NodeType::Element, args[0]->str);
      return wrap_node(node);
    });

    document->properties["createTextNode"] = make_function([](const std::vector<std::shared_ptr<Value>> &args) {
      if (args.empty() || args[0]->type != ValueType::String) return make_null();
      auto node = std::make_shared<Node>(NodeType::Text, args[0]->str);
      return wrap_node(node);
    });

    console->properties["log"] = make_function([](const std::vector<std::shared_ptr<Value>> &args) {
      std::cout << "[JS LOG] ";
      for (const auto &a : args) {
        if (!a) continue;
        if (a->type == ValueType::Number) std::cout << a->num << " ";
        else if (a->type == ValueType::String) std::cout << a->str << " ";
        else if (a->type == ValueType::Undefined) std::cout << "undefined ";
        else if (a->type == ValueType::Null) std::cout << "null ";
        else std::cout << "[Object] ";
      }
      std::cout << "\n";
      return make_undefined();
    });
  }

  std::shared_ptr<Value> last_val = make_undefined();
  for (auto &node : ast) {
    last_val = node->eval(env);
  }
  return last_val;
}

} // namespace js
