// css_values.cpp — CSS value parsing helpers
// Moved from layout.cpp (Split B)
#include "css_values.h"
#include <algorithm>
#include <string>

/* ── parse_px ────────────────────────────────────────────────────────────── */

float parse_px(const std::string &v, float relative_to) {
  if (v.empty() || v == "auto")
    return 0;

  // calc() — evaluate with the real containing-block size
  if (v.size() > 5 && v.substr(0, 5) == "calc(") {
    size_t end = v.rfind(')');
    if (end == std::string::npos)
      return 0;
    std::string expr = v.substr(5, end - 5);

    auto to_val = [&](const std::string &tok) -> float {
      if (tok.empty())
        return 0.f;
      try {
        if (tok.size() > 2 && tok.substr(tok.size() - 2) == "px")
          return std::stof(tok);
        if (tok.size() > 2 && tok.substr(tok.size() - 2) == "em")
          return std::stof(tok) * 16.f;
        if (tok.size() > 3 && tok.substr(tok.size() - 3) == "rem")
          return std::stof(tok) * 16.f;
        if (tok.size() > 2 && tok.substr(tok.size() - 2) == "vw") {
          extern int buffer_width;
          return std::stof(tok.substr(0, tok.size() - 2)) * buffer_width /
                 100.f;
        }
        if (tok.size() > 2 && tok.substr(tok.size() - 2) == "vh") {
          extern int g_viewport_height;
          return std::stof(tok.substr(0, tok.size() - 2)) *
                 (g_viewport_height > 0 ? g_viewport_height : 768) / 100.f;
        }
        if (!tok.empty() && tok.back() == '%')
          return std::stof(tok.substr(0, tok.size() - 1)) * relative_to / 100.f;
        return std::stof(tok);
      } catch (...) {
        return 0.f;
      }
    };

    std::vector<std::string> tokens;
    std::string tok;
    for (char c : expr) {
      if (c == ' ') {
        if (!tok.empty()) {
          tokens.push_back(tok);
          tok.clear();
        }
      } else
        tok += c;
    }
    if (!tok.empty())
      tokens.push_back(tok);

    float acc = 0.f;
    char op = '+';
    bool first = true;
    for (auto &t : tokens) {
      if (t.size() == 1 &&
          (t[0] == '+' || t[0] == '-' || t[0] == '*' || t[0] == '/')) {
        op = t[0];
      } else {
        float val = to_val(t);
        if (first) {
          acc = val;
          first = false;
        } else {
          switch (op) {
          case '+':
            acc += val;
            break;
          case '-':
            acc -= val;
            break;
          case '*':
            acc *= val;
            break;
          case '/':
            if (val)
              acc /= val;
            break;
          }
        }
        op = '+';
      }
    }
    return acc;
  }

  // clamp(min, val, max)
  if (v.size() > 6 && v.substr(0, 6) == "clamp(") {
    size_t end = v.rfind(')');
    if (end != std::string::npos) {
      std::string inner = v.substr(6, end - 6);
      // Split by commas (simple: no nested parens expected in basic usage)
      std::vector<std::string> args;
      std::string cur;
      int depth = 0;
      for (char c : inner) {
        if (c == '(') {
          depth++;
          cur += c;
        } else if (c == ')') {
          depth--;
          cur += c;
        } else if (c == ',' && depth == 0) {
          // trim
          size_t s2 = cur.find_first_not_of(" \t");
          size_t e2 = cur.find_last_not_of(" \t");
          if (s2 != std::string::npos)
            args.push_back(cur.substr(s2, e2 - s2 + 1));
          else
            args.push_back("");
          cur.clear();
        } else
          cur += c;
      }
      if (!cur.empty()) {
        size_t s2 = cur.find_first_not_of(" \t");
        size_t e2 = cur.find_last_not_of(" \t");
        args.push_back(s2 != std::string::npos ? cur.substr(s2, e2 - s2 + 1)
                                               : "");
      }
      if (args.size() == 3) {
        float mn = parse_px(args[0], relative_to);
        float val = parse_px(args[1], relative_to);
        float mx = parse_px(args[2], relative_to);
        if (val < mn)
          val = mn;
        if (val > mx)
          val = mx;
        return val;
      }
    }
  }

  // min(a, b) and max(a, b)
  if (v.size() > 4 && (v.substr(0, 4) == "min(" || v.substr(0, 4) == "max(")) {
    bool is_min = v[1] == 'i';
    size_t end = v.rfind(')');
    if (end != std::string::npos) {
      std::string inner = v.substr(4, end - 4);
      size_t comma = inner.find(',');
      if (comma != std::string::npos) {
        float a = parse_px(inner.substr(0, comma), relative_to);
        float b = parse_px(inner.substr(comma + 1), relative_to);
        return is_min ? std::min(a, b) : std::max(a, b);
      }
    }
  }

  try {
    if (v.back() == '%') {
      return (relative_to * std::stof(v.substr(0, v.length() - 1))) / 100.0f;
    }
    // vw / vh units (viewport-relative)
    extern int g_viewport_height;
    extern int buffer_width;
    if (v.size() > 2 && v.substr(v.size() - 2) == "vw")
      return std::stof(v.substr(0, v.size() - 2)) * buffer_width / 100.f;
    if (v.size() > 2 && v.substr(v.size() - 2) == "vh")
      return std::stof(v.substr(0, v.size() - 2)) *
             (g_viewport_height > 0 ? g_viewport_height : 768) / 100.f;
    if (v.size() > 4 && v.substr(v.size() - 4) == "vmin") {
      float vp_min = (float)std::min(
          buffer_width, g_viewport_height > 0 ? g_viewport_height : 768);
      return std::stof(v.substr(0, v.size() - 4)) * vp_min / 100.f;
    }
    if (v.size() > 4 && v.substr(v.size() - 4) == "vmax") {
      float vp_max = (float)std::max(
          buffer_width, g_viewport_height > 0 ? g_viewport_height : 768);
      return std::stof(v.substr(0, v.size() - 4)) * vp_max / 100.f;
    }
    // ch unit — approximate as 0.5 * font-size (default 16px → 8px)
    if (v.size() > 2 && v.substr(v.size() - 2) == "ch")
      return std::stof(v.substr(0, v.size() - 2)) * 8.f;
    return std::stof(v);
  } catch (...) {
    return 0;
  }
}

/* ── style_value ─────────────────────────────────────────────────────────── */

float style_value(const PropertyMap &map, const std::string &key,
                  float def, float relative_to) {
  auto it = map.find(key);
  if (it == map.end())
    return def;
  return parse_px(it->second, relative_to);
}

/* ── get_font_size ───────────────────────────────────────────────────────── */

int get_font_size(const std::shared_ptr<StyledNode> &node) {
  if (!node)
    return 16;
  std::string fs = node->value("font-size");
  if (fs.empty()) {
    if (node->node && node->node->type == NodeType::Element) {
      std::string tag = node->node->data;
      if (tag == "h1")
        return 32;
      if (tag == "h2")
        return 24;
      if (tag == "h3")
        return 20;
      if (tag == "h4")
        return 18;
      if (tag == "small")
        return 14;
      if (tag == "sub" || tag == "sup")
        return 12;
    }
    return 16;
  }

  // Handle named sizes
  if (fs == "xx-small")
    return 9;
  if (fs == "x-small")
    return 10;
  if (fs == "small")
    return 13;
  if (fs == "medium")
    return 16;
  if (fs == "large")
    return 18;
  if (fs == "x-large")
    return 24;
  if (fs == "xx-large")
    return 32;

  // Handle px/pt/em/rem/% units
  try {
    if (fs.size() > 2 && fs.substr(fs.size() - 2) == "px")
      return (int)std::stof(fs);
    if (fs.size() > 2 && fs.substr(fs.size() - 2) == "pt")
      return (int)(std::stof(fs) * 1.333f);
    if (fs.size() > 2 && fs.substr(fs.size() - 2) == "em")
      return (int)(std::stof(fs) * 16.f);
    if (fs.size() > 3 && fs.substr(fs.size() - 3) == "rem")
      return (int)(std::stof(fs) * 16.f);
    if (!fs.empty() && fs.back() == '%')
      return (int)(std::stof(fs.substr(0, fs.size() - 1)) * 16.f / 100.f);
  } catch (...) {
  }

  int size = atoi(fs.c_str());
  return size > 0 ? size : 16;
}

/* ── get_font_weight_bold ────────────────────────────────────────────────── */

bool get_font_weight_bold(const std::shared_ptr<StyledNode> &node) {
  if (!node)
    return false;
  std::string fw = node->value("font-weight");
  if (fw == "bold" || fw == "700" || fw == "800" || fw == "900")
    return true;

  if (node->node && node->node->type == NodeType::Element) {
    std::string tag = node->node->data;
    if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "b" ||
        tag == "strong")
      return true;
  }
  return false;
}

/* ── get_font_italic ─────────────────────────────────────────────────────── */

bool get_font_italic(const std::shared_ptr<StyledNode> &node) {
  if (!node) return false;
  std::string fs = node->value("font-style");
  if (fs == "italic" || fs == "oblique") return true;
  if (node->node && node->node->type == NodeType::Element) {
    std::string tag = node->node->data;
    if (tag == "em" || tag == "i" || tag == "cite" || tag == "var" || tag == "dfn")
      return true;
  }
  return false;
}
