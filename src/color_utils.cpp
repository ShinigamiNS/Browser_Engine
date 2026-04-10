// color_utils.cpp — CSS color name table and parse_color() implementation
// Moved from paint.cpp (Split A)
#include "paint.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

// Map CSS color strings to raw DIB format hex colors (0x00RRGGBB)
uint32_t parse_color(const std::string &color_str) {
  if (color_str.empty())
    return 0x00000000;

  std::string lower_color = color_str;
  // Trim leading/trailing whitespace (CSS values often have trailing spaces)
  size_t s = lower_color.find_first_not_of(" \t\r\n");
  size_t e = lower_color.find_last_not_of(" \t\r\n");
  if (s == std::string::npos) return 0x00000000;
  lower_color = lower_color.substr(s, e - s + 1);

  std::transform(lower_color.begin(), lower_color.end(), lower_color.begin(), ::tolower);

  if (lower_color == "red")    return 0x00FF0000;
  if (lower_color == "green")  return 0x0000FF00;
  if (lower_color == "blue")   return 0x000000FF;
  if (lower_color == "black")  return 0x00000000;
  if (lower_color == "white")  return 0x00FFFFFF;
  if (lower_color == "yellow") return 0x00FFFF00;
  if (lower_color == "purple") return 0x00800080;
  if (lower_color == "orange") return 0x00FF6600;
  if (lower_color == "pink")   return 0x00FF69B4;
  if (lower_color == "brown")  return 0x00A52A2A;
  if (lower_color == "cyan")   return 0x0000FFFF;
  if (lower_color == "magenta")return 0x00FF00FF;
  if (lower_color == "lime")   return 0x0000FF00;
  if (lower_color == "navy")   return 0x00000080;
  if (lower_color == "teal")   return 0x00008080;
  if (lower_color == "silver") return 0x00C0C0C0;
  if (lower_color == "grey" || lower_color == "gray") return 0x00808080;
  if (lower_color == "darkgray" || lower_color == "darkgrey") return 0x00A9A9A9;
  if (lower_color == "lightgray" || lower_color == "lightgrey") return 0x00D3D3D3;
  if (lower_color == "transparent") return 0x01000000;
  if (lower_color == "whitesmoke")  return 0x00F5F5F5;
  if (lower_color == "gainsboro")   return 0x00DCDCDC;
  if (lower_color == "ghostwhite")  return 0x00F8F8FF;
  if (lower_color == "aliceblue")   return 0x00F0F8FF;
  if (lower_color == "azure")       return 0x00F0FFFF;
  if (lower_color == "mintcream")   return 0x00F5FFFA;
  if (lower_color == "honeydew")    return 0x00F0FFF0;
  if (lower_color == "ivory")       return 0x00FFFFF0;
  if (lower_color == "linen")       return 0x00FAF0E6;
  if (lower_color == "seashell")    return 0x00FFF5EE;
  if (lower_color == "floralwhite") return 0x00FFFAF0;
  if (lower_color == "oldlace")     return 0x00FDF5E6;
  if (lower_color == "cornsilk")    return 0x00FFF8DC;
  if (lower_color == "lightyellow") return 0x00FFFFE0;
  if (lower_color == "lemonchiffon")return 0x00FFFACD;
  if (lower_color == "khaki")       return 0x00F0E68C;
  if (lower_color == "goldenrod")   return 0x00DAA520;
  if (lower_color == "gold")        return 0x00FFD700;
  if (lower_color == "wheat")       return 0x00F5DEB3;
  if (lower_color == "tan")         return 0x00D2B48C;
  if (lower_color == "burlywood")   return 0x00DEB887;
  if (lower_color == "sandybrown")  return 0x00F4A460;
  if (lower_color == "peru")        return 0x00CD853F;
  if (lower_color == "chocolate")   return 0x00D2691E;
  if (lower_color == "sienna")      return 0x00A0522D;
  if (lower_color == "saddlebrown") return 0x008B4513;
  if (lower_color == "maroon")      return 0x00800000;
  if (lower_color == "darkred")     return 0x008B0000;
  if (lower_color == "firebrick")   return 0x00B22222;
  if (lower_color == "crimson")     return 0x00DC143C;
  if (lower_color == "tomato")      return 0x00FF6347;
  if (lower_color == "coral")       return 0x00FF7F50;
  if (lower_color == "salmon")      return 0x00FA8072;
  if (lower_color == "darksalmon")  return 0x00E9967A;
  if (lower_color == "lightsalmon") return 0x00FFA07A;
  if (lower_color == "orangered")   return 0x00FF4500;
  if (lower_color == "darkorange")  return 0x00FF8C00;
  if (lower_color == "peachpuff")   return 0x00FFDAB9;
  if (lower_color == "bisque")      return 0x00FFE4C4;
  if (lower_color == "moccasin")    return 0x00FFE4B5;
  if (lower_color == "mistyrose")   return 0x00FFE4E1;
  if (lower_color == "lavender")    return 0x00E6E6FA;
  if (lower_color == "lavenderblush")return 0x00FFF0F5;
  if (lower_color == "thistle")     return 0x00D8BFD8;
  if (lower_color == "plum")        return 0x00DDA0DD;
  if (lower_color == "violet")      return 0x00EE82EE;
  if (lower_color == "orchid")      return 0x00DA70D6;
  if (lower_color == "fuchsia")     return 0x00FF00FF;
  if (lower_color == "indigo")      return 0x004B0082;
  if (lower_color == "darkviolet")  return 0x009400D3;
  if (lower_color == "mediumpurple")return 0x009370DB;
  if (lower_color == "blueviolet")  return 0x008A2BE2;
  if (lower_color == "darkblue")    return 0x0000008B;
  if (lower_color == "mediumblue")  return 0x000000CD;
  if (lower_color == "royalblue")   return 0x004169E1;
  if (lower_color == "dodgerblue")  return 0x001E90FF;
  if (lower_color == "deepskyblue") return 0x0000BFFF;
  if (lower_color == "cornflowerblue") return 0x006495ED;
  if (lower_color == "steelblue")   return 0x004682B4;
  if (lower_color == "lightblue")   return 0x00ADD8E6;
  if (lower_color == "skyblue")     return 0x0087CEEB;
  if (lower_color == "lightskyblue")return 0x0087CEFA;
  if (lower_color == "cadetblue")   return 0x005F9EA0;
  if (lower_color == "aquamarine")  return 0x007FFFD4;
  if (lower_color == "turquoise")   return 0x0040E0D0;
  if (lower_color == "mediumturquoise") return 0x0048D1CC;
  if (lower_color == "darkturquoise")   return 0x0000CED1;
  if (lower_color == "lightseagreen")   return 0x0020B2AA;
  if (lower_color == "mediumseagreen")  return 0x003CB371;
  if (lower_color == "seagreen")    return 0x002E8B57;
  if (lower_color == "darkgreen")   return 0x00006400;
  if (lower_color == "forestgreen") return 0x00228B22;
  if (lower_color == "limegreen")   return 0x0032CD32;
  if (lower_color == "yellowgreen") return 0x009ACD32;
  if (lower_color == "chartreuse")  return 0x007FFF00;
  if (lower_color == "lightgreen")  return 0x0090EE90;
  if (lower_color == "palegreen")   return 0x0098FB98;
  if (lower_color == "springgreen") return 0x0000FF7F;
  if (lower_color == "aqua")        return 0x0000FFFF;
  if (lower_color == "darkslategray" || lower_color == "darkslategrey") return 0x002F4F4F;
  if (lower_color == "slategray" || lower_color == "slategrey") return 0x00708090;
  if (lower_color == "lightslategray" || lower_color == "lightslategrey") return 0x00778899;
  if (lower_color == "dimgray" || lower_color == "dimgrey") return 0x00696969;
  if (lower_color == "currentcolor") return 0x00000000; // fallback to black

  // Support CSS Hex codes (#RRGGBB or #RGB)
  if (lower_color.length() > 0 && lower_color[0] == '#') {
    std::string hex = lower_color.substr(1);
    // Expand 3-char: #abc -> #aabbcc
    if (hex.length() == 3) {
      hex = std::string(2, hex[0]) + std::string(2, hex[1]) + std::string(2, hex[2]);
    }
    if (hex.length() == 6 || hex.length() == 8) {
      uint32_t parsed = 0;
      try { parsed = (uint32_t)std::stoul(hex.substr(0, 6), nullptr, 16); }
      catch (...) {}
      return parsed;
    }
  }

  // Handle background shorthand that may contain extra tokens like url(), position, etc.
  // Scan for a recognisable color token among space-separated parts.
  if (lower_color.find(' ') != std::string::npos) {
    std::stringstream scan(lower_color);
    std::string tok;
    while (scan >> tok) {
      if (tok[0] == '#' || tok.rfind("rgb(", 0) == 0 || tok.rfind("rgba(", 0) == 0)
        return parse_color(tok);
      if (tok == "white") return 0x00FFFFFF;
      if (tok == "black") return 0x00000000;
      if (tok == "red")   return 0x00FF0000;
    }
    return 0x01000000; // no color token found -> transparent
  }

  // rgb(r, g, b) / rgb(r g b) / rgba(r, g, b, a) parsing
  if (lower_color.size() >= 4 &&
      (lower_color.substr(0,4) == "rgb(" || lower_color.substr(0,5) == "rgba(")) {
    bool is_rgba = lower_color[3] == 'a';
    size_t p1 = lower_color.find('(');
    size_t p2 = lower_color.rfind(')');
    if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1) {
      std::string inner = lower_color.substr(p1+1, p2-p1-1);
      // Replace commas with spaces
      for (char &cc : inner) if (cc == ',') cc = ' ';
      std::istringstream iss(inner);
      float r=0, g=0, b=0, a=1.0f;
      iss >> r >> g >> b;
      if (is_rgba) iss >> a;
      if (a < 0.01f) return 0x01000000; // fully transparent
      return (((int)r & 0xFF) << 16) | (((int)g & 0xFF) << 8) | ((int)b & 0xFF);
    }
  }

  // Unknown color — return transparent rather than black to avoid dark blobs
  return 0x01000000;
}
