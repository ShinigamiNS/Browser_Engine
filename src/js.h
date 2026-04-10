#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>


namespace js {

enum class TokenType {
  Identifier,
  Number,
  String,
  Keyword,
  Operator,
  Punctuation,
  Eof
};

struct Token {
  TokenType type;
  std::string value;
};

std::vector<Token> tokenize(const std::string &input);

enum class ValueType { Undefined, Null, Number, String, Function, Object };

struct Value;

using NativeFunction = std::function<std::shared_ptr<Value>(
    const std::vector<std::shared_ptr<Value>> &)>;

struct Value {
  ValueType type;
  double num = 0;
  std::string str;
  NativeFunction native_func;
  
  // Object properties
  std::map<std::string, std::shared_ptr<Value>> properties;
  
  // Custom getters/setters
  std::map<std::string, NativeFunction> getters;
  std::map<std::string, std::function<void(std::shared_ptr<Value>)>> setters;
  
  // For DOM bindings (stores std::shared_ptr<Node>)
  std::shared_ptr<void> internal_ptr;
};

std::shared_ptr<Value> make_undefined();
std::shared_ptr<Value> make_null();
std::shared_ptr<Value> make_number(double n);
std::shared_ptr<Value> make_string(const std::string &s);
std::shared_ptr<Value> make_function(NativeFunction f);
std::shared_ptr<Value> make_object();

struct Environment {
  std::map<std::string, std::shared_ptr<Value>> vars;
  std::shared_ptr<Environment> parent;
  std::shared_ptr<Value> global_obj;

  std::shared_ptr<Value> get(const std::string &name);
  void set(const std::string &name, std::shared_ptr<Value> val);
};

struct ASTNode {
  virtual ~ASTNode() = default;
  virtual std::shared_ptr<Value> eval(std::shared_ptr<Environment> env) = 0;
};

std::vector<std::shared_ptr<ASTNode>> parse(const std::vector<Token> &tokens);
std::shared_ptr<Value> run_js(const std::string &source, std::shared_ptr<Environment> env = nullptr);

} // namespace js
