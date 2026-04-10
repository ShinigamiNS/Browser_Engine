#include "paint.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

// parse_color() moved to color_utils.cpp (Split A)

// ---------------------------------------------------------------------------
//  CSS linear-gradient parser
//  Fills angle_out (CSS degrees: 0=to top, 90=to right, 180=to bottom)
//  and stops_out (sorted by position, positions in [0,1]).
// ---------------------------------------------------------------------------
static void parse_gradient(const std::string &bgi,
                           float &angle_out,
                           std::vector<GradientStop> &stops_out) {
  angle_out = 180.f; // default: to bottom
  stops_out.clear();

  size_t fn_start = bgi.find("linear-gradient");
  if (fn_start == std::string::npos) return;
  size_t open = bgi.find('(', fn_start);
  if (open == std::string::npos) return;

  // Tokenize content inside outermost parens by comma, respecting depth
  std::vector<std::string> parts;
  {
    std::string cur;
    int depth = 0;
    for (size_t i = open + 1; i < bgi.size(); ++i) {
      char c = bgi[i];
      if (c == '(') { depth++; cur += c; }
      else if (c == ')') {
        if (depth == 0) break;
        depth--; cur += c;
      } else if (c == ',' && depth == 0) {
        size_t ts = cur.find_first_not_of(" \t");
        size_t te = cur.find_last_not_of(" \t");
        if (ts != std::string::npos) parts.push_back(cur.substr(ts, te - ts + 1));
        cur.clear();
      } else cur += c;
    }
    if (!cur.empty()) {
      size_t ts = cur.find_first_not_of(" \t");
      size_t te = cur.find_last_not_of(" \t");
      if (ts != std::string::npos) parts.push_back(cur.substr(ts, te - ts + 1));
    }
  }
  if (parts.empty()) return;

  // Parse direction/angle from first token
  int stop_start = 0;
  {
    const std::string &first = parts[0];
    bool is_dir = false;
    if (first.find("deg") != std::string::npos) {
      try { angle_out = std::stof(first); } catch (...) {}
      is_dir = true;
    } else if (first.find("turn") != std::string::npos) {
      try { angle_out = std::stof(first) * 360.f; } catch (...) {}
      is_dir = true;
    } else if (first.find("rad") != std::string::npos &&
               first.find("grad") == std::string::npos) {
      try { angle_out = std::stof(first) * 57.295779f; } catch (...) {}
      is_dir = true;
    } else if (first.find("grad") != std::string::npos) {
      try { angle_out = std::stof(first) * 0.9f; } catch (...) {}
      is_dir = true;
    } else if (first.size() >= 2 && first.substr(0, 2) == "to") {
      is_dir = true;
      if      (first.find("bottom") != std::string::npos && first.find("right") != std::string::npos) angle_out = 135.f;
      else if (first.find("bottom") != std::string::npos && first.find("left")  != std::string::npos) angle_out = 225.f;
      else if (first.find("top")    != std::string::npos && first.find("right") != std::string::npos) angle_out = 45.f;
      else if (first.find("top")    != std::string::npos && first.find("left")  != std::string::npos) angle_out = 315.f;
      else if (first.find("bottom") != std::string::npos) angle_out = 180.f;
      else if (first.find("top")    != std::string::npos) angle_out = 0.f;
      else if (first.find("right")  != std::string::npos) angle_out = 90.f;
      else if (first.find("left")   != std::string::npos) angle_out = 270.f;
    }
    if (is_dir) stop_start = 1;
  }

  // Parse color stops: each part may be "color" or "color pos%" or "color pos px"
  struct RawStop { std::string color_str; float pos; bool has_pos; };
  std::vector<RawStop> raw;
  for (int i = stop_start; i < (int)parts.size(); i++) {
    std::string p = parts[i];
    float pos = 0.f;
    bool has_pos = false;
    // A trailing percentage or px position may follow a space (but not inside rgb())
    size_t last_space = p.rfind(' ');
    if (last_space != std::string::npos) {
      std::string maybe_pos = p.substr(last_space + 1);
      if (!maybe_pos.empty()) {
        if (maybe_pos.back() == '%') {
          try { pos = std::stof(maybe_pos) / 100.f; has_pos = true; p = p.substr(0, last_space); } catch (...) {}
        } else if (maybe_pos.size() > 2 && maybe_pos.substr(maybe_pos.size() - 2) == "px") {
          // px positions on stops — ignore (we only support % positions for now)
          p = p.substr(0, last_space);
        }
      }
    }
    size_t ts = p.find_first_not_of(" \t");
    size_t te = p.find_last_not_of(" \t");
    if (ts != std::string::npos) p = p.substr(ts, te - ts + 1);
    if (!p.empty()) raw.push_back({p, pos, has_pos});
  }
  if (raw.size() < 2) return;

  // Distribute positions: first=0%, last=100%, fill gaps evenly
  if (!raw[0].has_pos)      { raw[0].pos = 0.f;  raw[0].has_pos = true; }
  if (!raw.back().has_pos)  { raw.back().pos = 1.f; raw.back().has_pos = true; }
  // Linear interpolation for unpositioned stops
  int i = 0;
  while (i < (int)raw.size()) {
    if (!raw[i].has_pos) {
      // Find bracketing positioned stops
      int prev = i - 1; // always has_pos (first stop was set above)
      int next = i + 1;
      while (next < (int)raw.size() && !raw[next].has_pos) next++;
      // next is guaranteed to have pos (last stop was set above)
      float p0 = raw[prev].pos, p1 = raw[next].pos;
      int n = next - prev; // number of segments
      for (int k = prev + 1; k < next; k++) {
        raw[k].pos = p0 + (p1 - p0) * (float)(k - prev) / (float)n;
        raw[k].has_pos = true;
      }
      i = next;
    } else {
      i++;
    }
  }

  // Convert raw stops to GradientStop
  for (const auto &rs : raw) {
    uint32_t c = parse_color(rs.color_str);
    if (c != 0 && c != 0x01000000) {
      GradientStop gs;
      gs.pos   = rs.pos;
      gs.color = c;
      stops_out.push_back(gs);
    }
  }
}

void render_layout_box(DisplayList &list, const std::shared_ptr<LayoutBox> &box,
                       uint32_t text_color, int inherited_font_size,
                       bool inherited_bold, bool inherited_underline = false,
                       bool inherited_italic = false,
                       const std::string &inherited_text_align = "",
                       float inherited_opacity = 1.0f,
                       float inherited_letter_spacing = 0.f,
                       bool parent_sticky = false,
                       float parent_sticky_top = 0.f,
                       float parent_sticky_orig_y = 0.f,
                       const std::string &inherited_td_line = "",
                       const std::string &inherited_td_color = "",
                       const std::string &inherited_td_style = "") {
  if (!box || !box->style_node)
    return;

  // visibility: hidden — box still takes space and nothing is painted for it,
  // BUT a descendant may override visibility:visible, so we must keep recursing.
  // We record the list size now and discard any commands this box pushes for
  // itself before recursing into children.
  size_t hidden_truncate_size = list.size();
  bool box_hidden = false;
  {
    std::string vis = box->style_node->value("visibility");
    if (vis == "hidden" || vis == "collapse") box_hidden = true;
  }

  // opacity
  float inherited_opacity_local = inherited_opacity;
  {
    std::string op = box->style_node->value("opacity");
    if (!op.empty()) {
      try {
        float ov = std::stof(op);
        if (ov < 0.f) ov = 0.f;
        if (ov > 1.f) ov = 1.f;
        inherited_opacity_local = inherited_opacity * ov;
      } catch (...) {}
    }
    if (inherited_opacity_local < 0.01f) return; // effectively invisible
  }

  std::string pos_val = box->style_node->value("position");
  bool is_fixed = pos_val == "fixed";
  bool is_sticky = pos_val == "sticky";

  // Sticky: compute sticky_top (default 0 if not specified)
  float sticky_top = 0.f;
  if (is_sticky) {
    std::string top_s = box->style_node->value("top");
    if (!top_s.empty() && top_s != "auto") {
      try { sticky_top = std::stof(top_s); } catch (...) {}
    }
  }

  // z-index
  int z_index = 0;
  {
    std::string zi = box->style_node->value("z-index");
    if (!zi.empty() && zi != "auto") {
      try { z_index = std::stoi(zi); } catch (...) {}
    }
  }

  // Ensure text inherits color downward
  std::string color_str = box->style_node->value("color");
  if (!color_str.empty()) {
    text_color = parse_color(color_str);
  }

  int font_size = inherited_font_size;
  bool bold = inherited_bold;
  bool underline = inherited_underline;
  bool italic = inherited_italic;
  std::string text_align = inherited_text_align;

  if (box->style_node->node &&
      box->style_node->node->type == NodeType::Element) {
    std::string tag = box->style_node->node->data;
    if (tag == "h1") {
      font_size = 32;
      bold = true;
    } else if (tag == "h2") {
      font_size = 24;
      bold = true;
    } else if (tag == "h3") {
      font_size = 20;
      bold = true;
    } else if (tag == "b" || tag == "strong") {
      bold = true;
    } else if (tag == "small") {
      font_size = 14;
    }
  }

  // Override with CSS styles if available
  std::string fw = box->style_node->value("font-weight");
  if (fw == "bold" || fw == "700" || fw == "800" || fw == "900")
    bold = true;
  else if (fw == "normal" || fw == "400")
    bold = false;

  std::string fs = box->style_node->value("font-size");
  if (!fs.empty()) {
    // Named sizes
    if (fs == "xx-small") font_size = 9;
    else if (fs == "x-small") font_size = 10;
    else if (fs == "small")   font_size = 13;
    else if (fs == "medium")  font_size = 16;
    else if (fs == "large")   font_size = 18;
    else if (fs == "x-large") font_size = 24;
    else if (fs == "xx-large")font_size = 32;
    else {
      try {
        if (fs.size()>2 && fs.substr(fs.size()-2)=="px") font_size=(int)std::stof(fs);
        else if (fs.size()>2 && fs.substr(fs.size()-2)=="pt") font_size=(int)(std::stof(fs)*1.333f);
        else if (fs.size()>3 && fs.substr(fs.size()-3)=="rem") font_size=(int)(std::stof(fs)*16.f);
        else if (fs.size()>2 && fs.substr(fs.size()-2)=="em") font_size=(int)(std::stof(fs)*16.f);
        else if (!fs.empty() && fs.back()=='%') font_size=(int)(std::stof(fs.substr(0,fs.size()-1))*16.f/100.f);
        else { int sz=(int)std::stof(fs); if(sz>0) font_size=sz; }
      } catch (...) {}
    }
    if (font_size < 6) font_size = 6;
  }

  // font-variant: small-caps — render uppercase at ~85% of font-size
  bool small_caps = false;
  {
    std::string fv = box->style_node->value("font-variant");
    if (fv == "small-caps" || fv == "all-small-caps") {
      small_caps = true;
      font_size = std::max(6, (int)(font_size * 0.85f));
    }
  }

  // font-family resolution
  std::string resolved_font = "Arial"; // default
  {
    std::string ff = box->style_node->value("font-family");
    if (!ff.empty()) {
      // Lowercase and strip quotes for comparison
      std::string ff_lower = ff;
      for (char &c : ff_lower) c = (char)::tolower((unsigned char)c);
      // Strip surrounding quotes from first family name
      size_t comma_pos = ff_lower.find(',');
      std::string first = (comma_pos != std::string::npos) ?
          ff_lower.substr(0, comma_pos) : ff_lower;
      // Trim whitespace and quotes
      size_t ts = first.find_first_not_of(" \t\"'");
      size_t te = first.find_last_not_of(" \t\"'");
      if (ts != std::string::npos) first = first.substr(ts, te - ts + 1);

      if (first == "monospace" || first == "courier" || first == "courier new" ||
          first == "consolas" || first == "lucida console" || first == "monaco") {
        resolved_font = "Courier New";
      } else if (first == "serif" || first == "times" || first == "times new roman" ||
                 first == "georgia" || first == "palatino") {
        resolved_font = "Times New Roman";
      } else if (first == "sans-serif" || first == "helvetica" || first == "verdana" ||
                 first == "tahoma" || first == "trebuchet ms" || first == "calibri" ||
                 first == "segoe ui" || first == "segoe") {
        resolved_font = "Arial";
      } else if (first == "impact" || first == "fantasy") {
        resolved_font = "Impact";
      } else if (first == "cursive") {
        resolved_font = "Comic Sans MS";
      } else if (first == "system-ui" || first == "-apple-system" ||
                 first == "blinkmacsystemfont" || first == "ui-sans-serif") {
        resolved_font = "Segoe UI";
      } else if (!first.empty() && first != "inherit" && first != "initial" &&
                 first != "unset") {
        // Try using the specified font name directly (may work on Windows)
        // Strip remaining quotes from original value
        std::string orig = ff;
        size_t c2 = orig.find(',');
        if (c2 != std::string::npos) orig = orig.substr(0, c2);
        size_t ots = orig.find_first_not_of(" \t\"'");
        size_t ote = orig.find_last_not_of(" \t\"'");
        if (ots != std::string::npos) resolved_font = orig.substr(ots, ote - ots + 1);
      }
    }
  }

  // text-decoration (shorthand + longhands) — start with inherited values
  std::string td_line = inherited_td_line;
  std::string td_color_str = inherited_td_color;
  std::string td_style = inherited_td_style;
  {
    std::string td = box->style_node->value("text-decoration");
    if (!td.empty() && td != "none") {
      if (td.find("underline")    != std::string::npos) td_line += " underline";
      if (td.find("overline")     != std::string::npos) td_line += " overline";
      if (td.find("line-through") != std::string::npos) td_line += " line-through";
      if (!td_line.empty()) underline = (td_line.find("underline") != std::string::npos);
    } else if (td == "none") {
      underline = false;
    }
    // longhands override shorthand
    std::string tdl = box->style_node->value("text-decoration-line");
    if (!tdl.empty() && tdl != "none") { td_line = tdl; }
    else if (tdl == "none") { td_line = "none"; underline = false; }
    std::string tdc = box->style_node->value("text-decoration-color");
    if (!tdc.empty()) td_color_str = tdc;
    std::string tds = box->style_node->value("text-decoration-style");
    if (!tds.empty()) td_style = tds;
    if (!td_line.empty() && td_line != "none")
      underline = (td_line.find("underline") != std::string::npos);
    // <a> tags default to underline
    if (!underline && box->style_node->node &&
        box->style_node->node->type == NodeType::Element &&
        box->style_node->node->data == "a" &&
        box->style_node->value("text-decoration").empty() && td_line.empty()) {
      underline = true;
      if (td_line.empty()) td_line = "underline";
    }
  }

  // CSS filter
  bool  has_filter        = false;
  float filter_blur       = 0.f;
  float filter_brightness = 1.f;
  float filter_contrast   = 1.f;
  float filter_grayscale  = 0.f;
  float filter_sepia      = 0.f;
  {
    std::string fstr = box->style_node->value("filter");
    if (!fstr.empty() && fstr != "none") {
      has_filter = true;
      // parse each function: blur(Npx) brightness(N) contrast(N) grayscale(N) sepia(N)
      size_t p = 0;
      while (p < fstr.size()) {
        while (p < fstr.size() && (fstr[p]==' '||fstr[p]=='\t')) p++;
        size_t name_start = p;
        while (p < fstr.size() && fstr[p] != '(') p++;
        if (p >= fstr.size()) break;
        std::string fname = fstr.substr(name_start, p - name_start);
        p++; // skip '('
        size_t arg_start = p;
        while (p < fstr.size() && fstr[p] != ')') p++;
        std::string arg = fstr.substr(arg_start, p - arg_start);
        if (p < fstr.size()) p++; // skip ')'
        // strip "px", "%"
        std::string num = arg;
        while (!num.empty() && (num.back()=='x'||num.back()=='p'||num.back()=='%'||num.back()==' ')) num.pop_back();
        float val = 0.f;
        try { val = std::stof(num); } catch(...) {}
        // normalize percentage to 0-1
        if (arg.find('%') != std::string::npos) val /= 100.f;
        if      (fname == "blur")       filter_blur       = val;
        else if (fname == "brightness") filter_brightness = val;
        else if (fname == "contrast")   filter_contrast   = val;
        else if (fname == "grayscale")  filter_grayscale  = std::min(1.f, val);
        else if (fname == "sepia")      filter_sepia      = std::min(1.f, val);
      }
    }
  }

  // font-style: italic
  {
    std::string fi = box->style_node->value("font-style");
    if (fi == "italic" || fi == "oblique")
      italic = true;
    else if (fi == "normal")
      italic = false;
    if (!italic && box->style_node->node &&
        box->style_node->node->type == NodeType::Element) {
      const std::string &tag2 = box->style_node->node->data;
      if (tag2 == "em" || tag2 == "i") italic = true;
    }
  }

  // text-align (inheritable)
  {
    std::string ta = box->style_node->value("text-align");
    if (!ta.empty()) text_align = ta;
  }

  // text-shadow (inheritable)
  float text_shadow_x = 0.f, text_shadow_y = 0.f, text_shadow_blur = 0.f;
  uint32_t text_shadow_c = 0;
  {
    std::string tsh = box->style_node->value("text-shadow");
    if (!tsh.empty() && tsh != "none") {
      std::istringstream tss(tsh);
      std::string tok;
      std::vector<std::string> parts;
      while (tss >> tok) parts.push_back(tok);
      int num_count = 0;
      for (const auto &pt : parts) {
        if (!pt.empty() && (::isdigit((unsigned char)pt[0]) || pt[0]=='-' || pt[0]=='.')) {
          float v = 0;
          try { v = std::stof(pt); } catch(...) {}
          if (num_count == 0) text_shadow_x = v;
          else if (num_count == 1) text_shadow_y = v;
          else if (num_count == 2) text_shadow_blur = v;
          num_count++;
        } else {
          uint32_t cc = parse_color(pt);
          if (cc != 0 && cc != 0x01000000) text_shadow_c = cc;
        }
      }
      if (text_shadow_c == 0 && num_count >= 2) text_shadow_c = 0x00888888;
    }
  }

  // letter-spacing (inheritable)
  float letter_spacing = inherited_letter_spacing;
  {
    std::string ls = box->style_node->value("letter-spacing");
    if (!ls.empty() && ls != "normal") {
      try {
        if (ls.size() > 2 && ls.substr(ls.size()-2) == "px")
          letter_spacing = std::stof(ls);
        else if (ls.size() > 2 && ls.substr(ls.size()-2) == "em")
          letter_spacing = std::stof(ls) * font_size;
        else
          letter_spacing = std::stof(ls);
      } catch (...) {}
    }
  }

  // Helper: parse a "Wpx style color" border shorthand token
  auto parse_border_shorthand = [](const std::string &s, float &out_w, uint32_t &out_c) {
    if (s.empty() || s == "none") return;
    std::stringstream ss(s);
    std::string w_tok, sty_tok, c_tok;
    ss >> w_tok >> sty_tok >> c_tok;
    if (!w_tok.empty()) {
      try { out_w = std::stof(w_tok); } catch (...) { out_w = 1.f; }
    }
    // color might be the 2nd or 3rd token (style keyword sits in between)
    if (!c_tok.empty())
      out_c = parse_color(c_tok);
    else if (!sty_tok.empty() && sty_tok[0] == '#')
      out_c = parse_color(sty_tok);
  };

  // Read per-edge border widths and colors
  float btw=0, brw=0, bbw=0, blw=0;
  uint32_t btc=0, brc=0, bbc=0, blc=0;
  {
    // Shorthand "border" applies to all 4 edges
    std::string bs = box->style_node->value("border");
    if (!bs.empty() && bs != "none")
      parse_border_shorthand(bs, btw, btc), brw=btw, bbw=btw, blw=btw,
      brc=btc, bbc=btc, blc=btc;

    // Per-side shorthands override
    std::string bts = box->style_node->value("border-top");
    if (!bts.empty() && bts != "none") parse_border_shorthand(bts, btw, btc);
    std::string brs = box->style_node->value("border-right");
    if (!brs.empty() && brs != "none") parse_border_shorthand(brs, brw, brc);
    std::string bbs = box->style_node->value("border-bottom");
    if (!bbs.empty() && bbs != "none") parse_border_shorthand(bbs, bbw, bbc);
    std::string bls = box->style_node->value("border-left");
    if (!bls.empty() && bls != "none") parse_border_shorthand(bls, blw, blc);

    // border-style: none suppresses all borders
    {
      std::string bstyle = box->style_node->value("border-style");
      if (bstyle == "none" || bstyle == "hidden") {
        btw = brw = bbw = blw = 0;
      }
    }
    // Per-edge border-style: none
    auto suppress_if_none = [&](const std::string &key, float &w) {
      std::string s = box->style_node->value(key);
      if (s == "none" || s == "hidden") w = 0;
    };
    suppress_if_none("border-top-style",    btw);
    suppress_if_none("border-right-style",  brw);
    suppress_if_none("border-bottom-style", bbw);
    suppress_if_none("border-left-style",   blw);

    // Explicit border-*-width longhands
    auto read_w = [&](const std::string &key, float &out) {
      std::string v = box->style_node->value(key);
      if (!v.empty() && v != "none") {
        if (v == "thin")   { out = 1.f; return; }
        if (v == "medium") { out = 3.f; return; }
        if (v == "thick")  { out = 5.f; return; }
        try { out = std::stof(v); } catch(...) { out = 1.f; }
      }
    };
    read_w("border-top-width",    btw);
    read_w("border-right-width",  brw);
    read_w("border-bottom-width", bbw);
    read_w("border-left-width",   blw);

    // Explicit border-*-color longhands
    auto read_c = [&](const std::string &key, uint32_t &out) {
      std::string v = box->style_node->value(key);
      if (!v.empty()) {
        if (v == "currentColor" || v == "currentcolor") out = text_color;
        else out = parse_color(v);
      }
    };
    read_c("border-top-color",    btc);
    read_c("border-right-color",  brc);
    read_c("border-bottom-color", bbc);
    read_c("border-left-color",   blc);

    // border-color shorthand
    std::string bcolor = box->style_node->value("border-color");
    if (!bcolor.empty()) {
      uint32_t c = parse_color(bcolor);
      if (btc == 0) btc = c;
      if (brc == 0) brc = c;
      if (bbc == 0) bbc = c;
      if (blc == 0) blc = c;
    }
  }

  // Read outline property
  float ow = 0; uint32_t oc = 0;
  {
    std::string outline_s = box->style_node->value("outline");
    if (!outline_s.empty() && outline_s != "none" && outline_s != "0") {
      std::stringstream oss(outline_s);
      std::string tok;
      while (oss >> tok) {
        if (tok == "none") { ow = 0; break; }
        if (tok.find("px") != std::string::npos) {
          try { ow = std::stof(tok); } catch (...) {}
        } else if (tok == "thin") { ow = 1; }
        else if (tok == "medium") { ow = 3; }
        else if (tok == "thick")  { ow = 5; }
        else if (!tok.empty() && (tok[0]=='#' || tok.find("rgb")==0 || ::isalpha((unsigned char)tok[0]))) {
          uint32_t cc = parse_color(tok);
          if (cc != 0) oc = cc;
        }
      }
      // If outline style (e.g. "solid") was specified but no width was
      // parsed, use the CSS default of "medium" (3px).  Only apply this
      // when a color *was* explicitly given — otherwise the shorthand
      // was something like "outline:0" or an unparseable value, and we
      // should NOT invent a visible outline.
      if (ow == 0 && oc != 0) ow = 3;
    }
    std::string ow_s = box->style_node->value("outline-width");
    if (!ow_s.empty() && ow_s != "none") {
      if (ow_s == "thin") ow = 1;
      else if (ow_s == "medium") ow = 3;
      else if (ow_s == "thick") ow = 5;
      else { try { ow = std::stof(ow_s); } catch (...) {} }
    }
    std::string oc_s = box->style_node->value("outline-color");
    if (!oc_s.empty()) { uint32_t cc = parse_color(oc_s); if (cc != 0) oc = cc; }
    if (ow > 0 && oc == 0) oc = 0x00000000; // default: black outline
  }
  float outline_offset = 0;
  {
    std::string oo_s = box->style_node->value("outline-offset");
    if (!oo_s.empty()) {
      try { outline_offset = std::stof(oo_s); } catch (...) {}
    }
  }

  // Read box-shadow
  float shadow_x=0, shadow_y=0, shadow_blur=0; uint32_t shadow_c=0;
  {
    std::string bsh = box->style_node->value("box-shadow");
    if (!bsh.empty() && bsh != "none") {
      // Parse: [inset] x y blur [spread] color
      // We ignore inset and spread; just extract x, y, blur, and color
      std::istringstream bss(bsh);
      std::string tok;
      std::vector<std::string> parts;
      while (bss >> tok) parts.push_back(tok);
      int num_count = 0;
      for (const auto &pt : parts) {
        if (pt == "inset") continue;
        if (!pt.empty() && (::isdigit((unsigned char)pt[0]) || pt[0]=='-' || pt[0]=='.')) {
          float v = 0;
          try { v = std::stof(pt); } catch(...) {}
          if (num_count == 0) shadow_x = v;
          else if (num_count == 1) shadow_y = v;
          else if (num_count == 2) shadow_blur = v;
          num_count++;
        } else {
          uint32_t cc = parse_color(pt);
          if (cc != 0 && cc != 0x01000000) shadow_c = cc;
        }
      }
      if (shadow_c == 0 && (shadow_x != 0 || shadow_y != 0)) shadow_c = 0x00888888;
    }
  }

  // CSS transform: parse translate() and scale()
  float transform_tx = 0.f, transform_ty = 0.f;
  float transform_sx = 1.f, transform_sy = 1.f;
  {
    std::string tf = box->style_node->value("transform");
    if (!tf.empty() && tf != "none") {
      // Parse each function: translate(x,y), translateX(x), translateY(y),
      //                       scale(sx,sy), scaleX(s), scaleY(s)
      size_t p = 0;
      while (p < tf.size()) {
        // Skip whitespace
        while (p < tf.size() && tf[p] == ' ') p++;
        // Find function name
        size_t fn_start = p;
        while (p < tf.size() && tf[p] != '(') p++;
        if (p >= tf.size()) break;
        std::string fn_name = tf.substr(fn_start, p - fn_start);
        // Trim whitespace from fn_name
        size_t fs = fn_name.find_first_not_of(" \t");
        size_t fe = fn_name.find_last_not_of(" \t");
        if (fs != std::string::npos) fn_name = fn_name.substr(fs, fe - fs + 1);

        p++; // skip '('
        size_t arg_start = p;
        int depth = 1;
        while (p < tf.size() && depth > 0) {
          if (tf[p] == '(') depth++;
          else if (tf[p] == ')') depth--;
          p++;
        }
        std::string args_str = tf.substr(arg_start, p - arg_start - 1);
        // Replace commas with spaces
        for (char &c : args_str) if (c == ',') c = ' ';
        std::istringstream iss(args_str);
        std::vector<std::string> args;
        std::string a;
        while (iss >> a) args.push_back(a);

        auto parse_len = [](const std::string &s) -> float {
          if (s.empty()) return 0.f;
          try {
            if (s.size() > 2 && s.substr(s.size()-2) == "px") return std::stof(s);
            if (s.size() > 1 && s.back() == '%') return 0.f; // % ignored for now
            return std::stof(s);
          } catch (...) { return 0.f; }
        };
        auto parse_scale = [](const std::string &s) -> float {
          if (s.empty()) return 1.f;
          try { return std::stof(s); } catch (...) { return 1.f; }
        };

        if (fn_name == "translate") {
          if (args.size() >= 1) transform_tx += parse_len(args[0]);
          if (args.size() >= 2) transform_ty += parse_len(args[1]);
        } else if (fn_name == "translateX") {
          if (args.size() >= 1) transform_tx += parse_len(args[0]);
        } else if (fn_name == "translateY") {
          if (args.size() >= 1) transform_ty += parse_len(args[0]);
        } else if (fn_name == "scale") {
          if (args.size() >= 1) transform_sx = parse_scale(args[0]);
          if (args.size() >= 2) transform_sy = parse_scale(args[1]);
          else if (args.size() == 1) transform_sy = transform_sx;
        } else if (fn_name == "scaleX") {
          if (args.size() >= 1) transform_sx = parse_scale(args[0]);
        } else if (fn_name == "scaleY") {
          if (args.size() >= 1) transform_sy = parse_scale(args[0]);
        }
        // rotate, skew, matrix — ignored for now
      }
    }
  }

  // Read text-overflow and white-space
  std::string text_overflow_val = box->style_node->value("text-overflow");
  std::string white_space_val   = box->style_node->value("white-space");
  bool has_ellipsis = (text_overflow_val == "ellipsis");
  bool has_nowrap   = (white_space_val == "nowrap" || white_space_val == "pre");

  // Draw Background
  std::string bg_color_str = box->style_node->value("background-color");
  if (bg_color_str.empty())
    bg_color_str = box->style_node->value("background");

  // background-image: url(...) or linear/radial/conic-gradient(...)
  std::string bg_image_url;
  std::vector<GradientStop> gradient_stops_local;
  float gradient_angle_local = 180.f;
  GradientType gradient_type_local = GradientType::None;
  float gradient_conic_from_local = 0.f;
  bool  gradient_radial_ellipse_local = false;

  // Helper: split top-level comma-separated args (respects nested parens)
  auto split_gradient_args = [](const std::string &bgi, size_t open) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (size_t i = open + 1; i < bgi.size(); i++) {
      char c = bgi[i];
      if (c == '(') { depth++; cur += c; }
      else if (c == ')') {
        if (depth == 0) break;
        depth--; cur += c;
      } else if (c == ',' && depth == 0) {
        size_t ts = cur.find_first_not_of(" \t");
        if (ts != std::string::npos) parts.push_back(cur.substr(ts));
        cur.clear();
      } else cur += c;
    }
    if (!cur.empty()) {
      size_t ts = cur.find_first_not_of(" \t");
      if (ts != std::string::npos) parts.push_back(cur.substr(ts));
    }
    return parts;
  };

  {
    std::string bgi = box->style_node->value("background-image");
    if (!bgi.empty() && bgi.find("url(") != std::string::npos) {
      size_t s = bgi.find("url(") + 4;
      size_t e = bgi.find(')', s);
      if (e != std::string::npos) {
        bg_image_url = bgi.substr(s, e - s);
        if (!bg_image_url.empty() && (bg_image_url.front() == '"' || bg_image_url.front() == '\''))
          bg_image_url = bg_image_url.substr(1, bg_image_url.size() - 2);
      }
    }
    if (!bgi.empty() && bgi.find("linear-gradient") != std::string::npos) {
      parse_gradient(bgi, gradient_angle_local, gradient_stops_local);
      gradient_type_local = GradientType::Linear;
      if (bg_color_str.empty() && !gradient_stops_local.empty())
        bg_color_str = "_gradient_";
    } else if (!bgi.empty() && bgi.find("radial-gradient") != std::string::npos) {
      size_t open = bgi.find('(');
      if (open != std::string::npos) {
        auto parts = split_gradient_args(bgi, open);
        // First part may be shape descriptor "circle" / "ellipse" / "circle at ..."
        size_t color_start = 0;
        if (!parts.empty()) {
          std::string first = parts[0];
          if (first.find("circle") != std::string::npos ||
              first.find("ellipse") != std::string::npos ||
              first.find("closest") != std::string::npos ||
              first.find("farthest") != std::string::npos ||
              first.find(" at ") != std::string::npos) {
            gradient_radial_ellipse_local = (first.find("ellipse") != std::string::npos);
            color_start = 1;
          }
        }
        for (size_t pi = color_start; pi < parts.size(); pi++) {
          GradientStop gs;
          std::string p = parts[pi];
          size_t sp = p.rfind(' ');
          std::string clr = p, pos_s;
          if (sp != std::string::npos) {
            clr = p.substr(0, sp);
            pos_s = p.substr(sp + 1);
          }
          uint32_t c = parse_color(clr);
          if (c == 0 && !clr.empty()) c = parse_color(p); // fallback: whole token
          float fpos = (parts.size() > 1) ?
              (float)(pi - color_start) / (float)(parts.size() - color_start - 1) : 0.f;
          if (!pos_s.empty()) {
            try {
              if (pos_s.back() == '%') fpos = std::stof(pos_s) / 100.f;
              else fpos = std::stof(pos_s) / 100.f;
            } catch (...) {}
          }
          gs.pos = fpos; gs.color = c;
          gradient_stops_local.push_back(gs);
        }
        gradient_type_local = GradientType::Radial;
        if (bg_color_str.empty() && !gradient_stops_local.empty())
          bg_color_str = "_gradient_";
      }
    } else if (!bgi.empty() && bgi.find("conic-gradient") != std::string::npos) {
      size_t open = bgi.find('(');
      if (open != std::string::npos) {
        auto parts = split_gradient_args(bgi, open);
        size_t color_start = 0;
        // First part may be "from Ndeg" or "from Ndeg at x y"
        if (!parts.empty()) {
          std::string first = parts[0];
          if (first.find("from") != std::string::npos) {
            size_t fdeg = first.find("deg");
            if (fdeg != std::string::npos) {
              size_t fstart = first.find("from");
              std::string deg_str = first.substr(fstart + 5, fdeg - fstart - 5);
              try { gradient_conic_from_local = std::stof(deg_str); } catch(...) {}
            }
            color_start = 1;
          }
        }
        for (size_t pi = color_start; pi < parts.size(); pi++) {
          GradientStop gs;
          std::string p = parts[pi];
          size_t sp = p.rfind(' ');
          std::string clr = p;
          if (sp != std::string::npos) clr = p.substr(0, sp);
          uint32_t c = parse_color(clr);
          if (c == 0 && !clr.empty()) c = parse_color(p);
          float fpos = (parts.size() > color_start + 1) ?
              (float)(pi - color_start) / (float)(parts.size() - color_start - 1) : 0.f;
          gs.pos = fpos; gs.color = c;
          gradient_stops_local.push_back(gs);
        }
        gradient_type_local = GradientType::Conic;
        if (bg_color_str.empty() && !gradient_stops_local.empty())
          bg_color_str = "_gradient_";
      }
    }
  }

  // Resolve currentColor references using inherited text color
  auto resolve_color = [&](const std::string &s) -> uint32_t {
    if (s == "currentColor" || s == "currentcolor") return text_color;
    return parse_color(s);
  };

  bool has_gradient = (gradient_stops_local.size() >= 2);
  bool has_bg = !bg_color_str.empty() || !bg_image_url.empty() || has_gradient ||
                btw > 0 || brw > 0 || bbw > 0 || blw > 0;

  // <img> elements — suppress all background/border rendering regardless of src.
  // The actual image pixels are blitted by main.cpp's blit_images_pass when the
  // image loads. Placeholder borders for failed/pending images clutter the page.
  if (has_bg && box->style_node->node &&
      box->style_node->node->type == NodeType::Element &&
      box->style_node->node->data == "img") {
    has_bg = false;
  }

  // Compute effective sticky state (may be inherited from sticky ancestor)
  bool eff_sticky = is_sticky || parent_sticky;
  float eff_sticky_top = is_sticky ? sticky_top :
                         (parent_sticky ? parent_sticky_top : 0.f);
  float eff_sticky_parent_orig = is_sticky ?
      box->dimensions.border_box().y :
      parent_sticky_orig_y;

  if (has_bg) {
    // Resolve solid background color (sentinel "_gradient_" → use first gradient stop)
    uint32_t bg;
    if (bg_color_str == "_gradient_") {
      bg = gradient_stops_local[0].color;
    } else {
      bg = bg_color_str.empty() ? 0x01000000 : resolve_color(bg_color_str);
    }
    bool transparent_bg = (bg == 0x01000000);

    if (!transparent_bg || btw > 0 || brw > 0 || bbw > 0 || blw > 0 ||
        !bg_image_url.empty() || has_gradient) {
      DisplayCommand cmd;
      cmd.type = DisplayCommandType::SolidColor;
      cmd.rect = box->dimensions.border_box();
      cmd.color = transparent_bg ? 0x01000000 : bg;
      cmd.fixed = is_fixed;
      cmd.z_index = z_index;
      // border-radius
      std::string br_str = box->style_node->value("border-radius");
      if (!br_str.empty() && br_str != "0" && br_str != "none") {
        try {
          float br_val = 0;
          if (br_str.back() == '%') {
            float pct = std::stof(br_str.substr(0, br_str.size()-1));
            float half_min = std::min(cmd.rect.width, cmd.rect.height) * 0.5f;
            br_val = pct * half_min / 100.f;
          } else if (br_str.size()>2 && br_str.substr(br_str.size()-2)=="px") {
            br_val = std::stof(br_str);
          } else {
            br_val = std::stof(br_str);
          }
          cmd.border_radius = br_val;
        } catch (...) {}
      }
      // Border color: CSS default is currentColor (text color) when unspecified,
      // but use a subtle grey as fallback to avoid harsh black borders.
      uint32_t border_fallback = 0x00DADCE0; // light grey fallback
      cmd.border_top_w = btw;    cmd.border_top_c    = btc ? btc : border_fallback;
      cmd.border_right_w = brw;  cmd.border_right_c  = brc ? brc : border_fallback;
      cmd.border_bottom_w = bbw; cmd.border_bottom_c = bbc ? bbc : border_fallback;
      cmd.border_left_w = blw;   cmd.border_left_c   = blc ? blc : border_fallback;
      cmd.bg_image_url = bg_image_url;
      if (!bg_image_url.empty()) {
        cmd.bg_size = box->style_node->value("background-size");
        cmd.bg_position = box->style_node->value("background-position");
        cmd.bg_repeat = box->style_node->value("background-repeat");
      }
      if (has_gradient) {
        cmd.gradient_type  = gradient_type_local;
        cmd.gradient_stops = gradient_stops_local;
        cmd.gradient_angle = gradient_angle_local;
        cmd.gradient_conic_from = gradient_conic_from_local;
        cmd.gradient_radial_ellipse = gradient_radial_ellipse_local;
      }
      cmd.outline_w = ow; cmd.outline_c = oc; cmd.outline_offset = outline_offset;
      cmd.shadow_x = shadow_x; cmd.shadow_y = shadow_y;
      cmd.shadow_blur = shadow_blur; cmd.shadow_c = shadow_c;
      cmd.opacity = inherited_opacity_local;
      cmd.transform_tx = transform_tx; cmd.transform_ty = transform_ty;
      cmd.transform_sx = transform_sx; cmd.transform_sy = transform_sy;
      // sticky
      cmd.sticky = eff_sticky;
      cmd.sticky_orig_y = cmd.rect.y;
      cmd.sticky_top = eff_sticky ? (cmd.rect.y + eff_sticky_top - eff_sticky_parent_orig) : 0.f;
      // filter
      cmd.has_filter        = has_filter;
      cmd.filter_blur       = filter_blur;
      cmd.filter_brightness = filter_brightness;
      cmd.filter_contrast   = filter_contrast;
      cmd.filter_grayscale  = filter_grayscale;
      cmd.filter_sepia      = filter_sepia;
      list.push_back(cmd);
    }
  } else if (ow > 0) {
    DisplayCommand cmd;
    cmd.type = DisplayCommandType::SolidColor;
    cmd.rect = box->dimensions.border_box();
    cmd.color = 0x01000000;
    cmd.fixed = is_fixed; cmd.z_index = z_index;
    cmd.outline_w = ow; cmd.outline_c = oc; cmd.outline_offset = outline_offset;
    cmd.opacity = inherited_opacity_local;
    cmd.transform_tx = transform_tx; cmd.transform_ty = transform_ty;
    cmd.transform_sx = transform_sx; cmd.transform_sy = transform_sy;
    cmd.sticky = eff_sticky;
    cmd.sticky_orig_y = cmd.rect.y;
    cmd.sticky_top = eff_sticky ? (cmd.rect.y + eff_sticky_top - eff_sticky_parent_orig) : 0.f;
    list.push_back(cmd);
  }

  // text-transform
  std::string text_transform = box->style_node->value("text-transform");
  // small-caps implies uppercase transform
  if (small_caps && text_transform.empty()) text_transform = "uppercase";

  // Helper: apply text-transform to a string
  auto apply_text_transform = [&](std::string t) -> std::string {
    if (text_transform == "uppercase") {
      for (char &c : t) c = (char)::toupper((unsigned char)c);
    } else if (text_transform == "lowercase") {
      for (char &c : t) c = (char)::tolower((unsigned char)c);
    } else if (text_transform == "capitalize") {
      bool new_word = true;
      for (char &c : t) {
        if (c == ' ' || c == '\t' || c == '\n') { new_word = true; }
        else if (new_word) { c = (char)::toupper((unsigned char)c); new_word = false; }
      }
    }
    return t;
  };

  if (box->style_node->node && box->style_node->node->type == NodeType::Text) {
    DisplayCommand cmd;
    cmd.type = DisplayCommandType::Text;
    cmd.rect = box->dimensions.content;
    cmd.text = apply_text_transform(box->style_node->node->data);
    cmd.color = text_color;
    cmd.font_size = font_size;
    cmd.bold = bold;
    cmd.underline = underline;
    cmd.italic = italic;
    cmd.text_align = text_align;
    cmd.fixed = is_fixed;
    cmd.z_index = z_index;
    cmd.ellipsis = has_ellipsis;
    cmd.nowrap   = has_nowrap;
    cmd.opacity  = inherited_opacity_local;
    cmd.letter_spacing = letter_spacing;
    cmd.transform_tx = transform_tx; cmd.transform_ty = transform_ty;
    // sticky (inherit from parent if needed)
    cmd.sticky = eff_sticky;
    cmd.sticky_orig_y = cmd.rect.y;
    cmd.sticky_top = eff_sticky ? (cmd.rect.y + eff_sticky_top - eff_sticky_parent_orig) : 0.f;
    cmd.text_shadow_x = text_shadow_x;
    cmd.text_shadow_y = text_shadow_y;
    cmd.text_shadow_blur = text_shadow_blur;
    cmd.text_shadow_c = text_shadow_c;
    cmd.font_family = resolved_font;
    // text-decoration longhands
    cmd.text_decoration_line  = td_line;
    cmd.text_decoration_color = td_color_str.empty() ? 0 : parse_color(td_color_str);
    cmd.text_decoration_style = td_style;
    if (box->extra_fragments.empty()) {
      list.push_back(cmd);
    } else {
      for (auto &ef : box->extra_fragments) {
        DisplayCommand ecmd = cmd;
        ecmd.rect = ef.rect;
        ecmd.text = apply_text_transform(ef.text);
        list.push_back(ecmd);
      }
    }
  } else if (box->style_node->node &&
             box->style_node->node->type == NodeType::Element) {
    std::string tag = box->style_node->node->data;

    // <li> — emit bullet or number before content
    if (tag == "li") {
      // list-style-type is now supplied by the UA stylesheet (ua_stylesheet.cpp),
      // including the nested ul → disc/circle/square cascade. Inheritance
      // through StyledNode::value() handles propagation from <ul>/<ol> to <li>.
      std::string lst = box->style_node->value("list-style-type");
      if (lst.empty()) lst = "disc";
      std::string marker;

      if (lst == "none") {
        marker = "";
      } else if (lst == "circle") {
        marker = "\xE2\x97\x8B "; // ○
      } else if (lst == "square") {
        marker = "\xE2\x96\xA0 "; // ■
      } else if (lst == "decimal" || lst == "lower-alpha" || lst == "upper-alpha" ||
                 lst == "lower-roman" || lst == "upper-roman") {
        // Count this li's position among siblings
        int index = 1;
        if (box->style_node->node) {
          auto parent_node = box->style_node->node->parent.lock();
          if (parent_node) {
            if (parent_node->attributes.count("start")) {
              try { index = std::stoi(parent_node->attributes.at("start")); } catch (...) {}
            }
            for (auto &sib : parent_node->children) {
              if (sib.get() == box->style_node->node.get()) break;
              if (sib->type == NodeType::Element && sib->data == "li") index++;
            }
          }
        }
        if (lst == "decimal") {
          marker = std::to_string(index) + ". ";
        } else if (lst == "lower-alpha") {
          marker = std::string(1, (char)('a' + ((index - 1) % 26))) + ". ";
        } else if (lst == "upper-alpha") {
          marker = std::string(1, (char)('A' + ((index - 1) % 26))) + ". ";
        } else if (lst == "lower-roman" || lst == "upper-roman") {
          // Simple roman numeral conversion
          std::string roman;
          int v = index;
          const int vals[] = {1000,900,500,400,100,90,50,40,10,9,5,4,1};
          const char* syms[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
          for (int ri = 0; ri < 13 && v > 0; ri++) {
            while (v >= vals[ri]) { roman += syms[ri]; v -= vals[ri]; }
          }
          if (lst == "lower-roman")
            for (char &c : roman) c = (char)tolower((unsigned char)c);
          marker = roman + ". ";
        }
      } else {
        marker = "\xE2\x80\xA2 "; // • default disc
      }

      if (box->dimensions.content.height < 2.f) marker = "";
      if (!marker.empty()) {
        DisplayCommand mcmd;
        mcmd.type = DisplayCommandType::Text;
        mcmd.rect = box->dimensions.content;
        mcmd.rect.x -= 18.f;
        mcmd.text = marker;
        mcmd.color = text_color;
        mcmd.font_size = font_size;
        mcmd.bold = bold;
        mcmd.fixed = is_fixed;
        mcmd.z_index = z_index;
        mcmd.opacity = inherited_opacity_local;
        mcmd.font_family = resolved_font;
        list.push_back(mcmd);
      }
    }

    if (tag == "progress") {
      // Draw track background
      DisplayCommand track_cmd;
      track_cmd.type = DisplayCommandType::SolidColor;
      track_cmd.rect = box->dimensions.border_box();
      track_cmd.color = 0x00E0E0E0;
      track_cmd.fixed = is_fixed; track_cmd.z_index = z_index;
      track_cmd.border_radius = track_cmd.rect.height * 0.5f;
      track_cmd.border_top_w = track_cmd.border_right_w =
      track_cmd.border_bottom_w = track_cmd.border_left_w = 1.f;
      track_cmd.border_top_c = track_cmd.border_right_c =
      track_cmd.border_bottom_c = track_cmd.border_left_c = 0x00C0C0C0;
      track_cmd.opacity = inherited_opacity_local;
      list.push_back(track_cmd);

      // Draw progress fill
      float value = 0.f, max_val = 1.f;
      if (box->style_node->node->attributes.count("value")) {
        try { value = std::stof(box->style_node->node->attributes.at("value")); } catch(...) {}
      }
      if (box->style_node->node->attributes.count("max")) {
        try { max_val = std::stof(box->style_node->node->attributes.at("max")); } catch(...) {}
        if (max_val <= 0) max_val = 1.f;
      }
      float pct = std::min(1.f, std::max(0.f, value / max_val));
      if (pct > 0.01f) {
        DisplayCommand fill_cmd;
        fill_cmd.type = DisplayCommandType::SolidColor;
        fill_cmd.rect = box->dimensions.border_box();
        fill_cmd.rect.width *= pct;
        fill_cmd.color = 0x002196F3; // Material blue
        fill_cmd.fixed = is_fixed; fill_cmd.z_index = z_index;
        fill_cmd.border_radius = fill_cmd.rect.height * 0.5f;
        fill_cmd.opacity = inherited_opacity_local;
        list.push_back(fill_cmd);
      }
    } else if (tag == "meter") {
      // Draw like progress bar
      float value = 0.f, max_val = 1.f;
      if (box->style_node->node->attributes.count("value")) {
        try { value = std::stof(box->style_node->node->attributes.at("value")); } catch(...) {}
      }
      if (box->style_node->node->attributes.count("max")) {
        try { max_val = std::stof(box->style_node->node->attributes.at("max")); } catch(...) {}
        if (max_val <= 0) max_val = 1.f;
      }
      float pct = std::min(1.f, std::max(0.f, value / max_val));
      DisplayCommand track_cmd;
      track_cmd.type = DisplayCommandType::SolidColor;
      track_cmd.rect = box->dimensions.border_box();
      track_cmd.color = 0x00E0E0E0;
      track_cmd.fixed = is_fixed; track_cmd.z_index = z_index;
      track_cmd.border_top_w = track_cmd.border_right_w =
      track_cmd.border_bottom_w = track_cmd.border_left_w = 1.f;
      track_cmd.border_top_c = track_cmd.border_right_c =
      track_cmd.border_bottom_c = track_cmd.border_left_c = 0x00C0C0C0;
      track_cmd.opacity = inherited_opacity_local;
      list.push_back(track_cmd);
      if (pct > 0.01f) {
        DisplayCommand fill_cmd;
        fill_cmd.type = DisplayCommandType::SolidColor;
        fill_cmd.rect = box->dimensions.border_box();
        fill_cmd.rect.width *= pct;
        fill_cmd.color = 0x004CAF50; // Material green
        fill_cmd.fixed = is_fixed; fill_cmd.z_index = z_index;
        fill_cmd.opacity = inherited_opacity_local;
        list.push_back(fill_cmd);
      }
    } else if (tag == "select") {
      // Render as a styled dropdown control
      DisplayCommand bg_cmd;
      bg_cmd.type = DisplayCommandType::SolidColor;
      bg_cmd.rect = box->dimensions.border_box();
      bg_cmd.color = 0x00FFFFFF;
      bg_cmd.fixed = is_fixed;
      bg_cmd.z_index = z_index;
      bg_cmd.border_top_w = bg_cmd.border_right_w =
      bg_cmd.border_bottom_w = bg_cmd.border_left_w = 1.f;
      bg_cmd.border_top_c = bg_cmd.border_right_c =
      bg_cmd.border_bottom_c = bg_cmd.border_left_c = 0x00A0A0A0;
      bg_cmd.opacity = inherited_opacity_local;
      list.push_back(bg_cmd);

      // Draw selected option text (first <option> or option with selected attr)
      std::string selected_text;
      for (const auto &child : box->style_node->node->children) {
        if (child && child->type == NodeType::Element && child->data == "option") {
          if (selected_text.empty() ||
              child->attributes.count("selected")) {
            // Get text content
            for (const auto &tc : child->children) {
              if (tc && tc->type == NodeType::Text) {
                selected_text = tc->data;
                // Trim
                size_t ts = selected_text.find_first_not_of(" \t\r\n");
                size_t te = selected_text.find_last_not_of(" \t\r\n");
                if (ts != std::string::npos) selected_text = selected_text.substr(ts, te-ts+1);
              }
            }
            if (child->attributes.count("selected")) break;
          }
        }
      }
      if (!selected_text.empty()) {
        DisplayCommand tcmd;
        tcmd.type = DisplayCommandType::Text;
        tcmd.rect = box->dimensions.content;
        tcmd.rect.x += 4;
        tcmd.text = selected_text;
        tcmd.color = 0x00000000;
        tcmd.font_size = font_size;
        tcmd.fixed = is_fixed;
        tcmd.z_index = z_index;
        tcmd.opacity = inherited_opacity_local;
        list.push_back(tcmd);
      }
      // Draw arrow indicator on right side
      DisplayCommand arrow_cmd;
      arrow_cmd.type = DisplayCommandType::Text;
      arrow_cmd.rect = box->dimensions.border_box();
      arrow_cmd.rect.x = arrow_cmd.rect.x + arrow_cmd.rect.width - 18;
      arrow_cmd.rect.width = 16;
      arrow_cmd.text = "\xE2\x96\xBC"; // ▼ UTF-8 down triangle
      arrow_cmd.color = 0x00666666;
      arrow_cmd.font_size = font_size > 10 ? font_size - 2 : font_size;
      arrow_cmd.fixed = is_fixed;
      arrow_cmd.z_index = z_index;
      arrow_cmd.opacity = inherited_opacity_local;
      list.push_back(arrow_cmd);
    } else if (tag == "input" || tag == "textarea" || tag == "button") {
      std::string type = "";
      if (box->style_node->node->attributes.count("type"))
        type = box->style_node->node->attributes.at("type");
      if (type == "hidden" || type == "checkbox" || type == "radio")
        return; // Don't draw hidden/checkbox/radio inputs here
      bool is_button = (tag == "button" || type == "submit" || type == "button" ||
                        type == "reset" || type == "image");

      // Only draw a UA fallback background if no CSS background was applied.
      // CSS background (has_bg path above) handles border-radius, color, borders.
      if (!has_bg) {
        if (is_button) {
          // Buttons: draw a visible UA box so they're recognizable
          DisplayCommand bg_cmd;
          bg_cmd.type = DisplayCommandType::SolidColor;
          bg_cmd.rect = box->dimensions.border_box();
          bg_cmd.color = 0x00E0E0E0;
          bg_cmd.fixed = is_fixed;
          bg_cmd.z_index = z_index;
          bg_cmd.border_top_w = bg_cmd.border_right_w =
          bg_cmd.border_bottom_w = bg_cmd.border_left_w = 1.f;
          bg_cmd.border_top_c = bg_cmd.border_right_c =
          bg_cmd.border_bottom_c = bg_cmd.border_left_c = 0x00A0A0A0;
          bg_cmd.opacity = inherited_opacity_local;
          list.push_back(bg_cmd);
        }
        // Non-button inputs/textareas: no UA border fallback.
        // Their parent containers typically provide borders/styling.
      }

      // Resolve value text for rendering inside the control.
      std::string val_text;
      if (is_button) {
        if (tag == "button") {
          // <button> gets its label from inner text nodes (recursive)
          // Skip <style>, <script>, <svg> children which contain non-visible text
          std::function<void(const std::shared_ptr<Node>&)> collect_text;
          collect_text = [&](const std::shared_ptr<Node> &n) {
            if (!n) return;
            if (n->type == NodeType::Text) val_text += n->data;
            if (n->type == NodeType::Element) {
              const std::string &t = n->data;
              if (t == "style" || t == "script" || t == "svg" || t == "noscript")
                return; // skip non-visual children
            }
            for (auto &c : n->children) collect_text(c);
          };
          collect_text(box->style_node->node);
          // Trim whitespace
          size_t s = val_text.find_first_not_of(" \t\r\n");
          size_t e = val_text.find_last_not_of(" \t\r\n");
          if (s != std::string::npos) val_text = val_text.substr(s, e - s + 1);
          else val_text.clear();
        } else {
          // <input type="submit/button/reset"> — use value attribute
          auto check_val = box->style_node->node->attributes.find("value");
          if (check_val != box->style_node->node->attributes.end())
            val_text = check_val->second;
        }
      } else if (tag == "input" || tag == "textarea") {
        // Render value for text-like inputs so users can see what they type.
        // Password inputs are masked with bullet characters.
        bool is_text_like = (type == "" || type == "text" || type == "search" ||
                             type == "url" || type == "email" || type == "tel" ||
                             type == "number" || type == "password");
        if (is_text_like) {
          if (tag == "textarea") {
            // <textarea> gets its value from inner text nodes
            std::function<void(const std::shared_ptr<Node>&)> collect_text;
            collect_text = [&](const std::shared_ptr<Node> &n) {
              if (!n) return;
              if (n->type == NodeType::Text) val_text += n->data;
              for (auto &c : n->children) collect_text(c);
            };
            collect_text(box->style_node->node);
          } else {
            auto check_val = box->style_node->node->attributes.find("value");
            if (check_val != box->style_node->node->attributes.end())
              val_text = check_val->second;
          }
          // Mask password values with bullet characters
          if (type == "password" && !val_text.empty()) {
            // U+25CF BLACK CIRCLE encoded as UTF-8: 0xE2 0x97 0x8F
            std::string masked;
            for (size_t i = 0; i < val_text.size(); ++i)
              masked += "\xE2\x97\x8F";
            val_text = masked;
          }
        }
      }

      if (!val_text.empty()) {
        // Draw value text inside the input
        DisplayCommand tcmd;
        tcmd.type = DisplayCommandType::Text;
        tcmd.rect = box->dimensions.content;
        tcmd.rect.x += 5;
        tcmd.text = val_text;
        tcmd.color = text_color;
        tcmd.font_size = font_size;
        tcmd.bold = bold;
        tcmd.fixed = is_fixed;
        tcmd.z_index = z_index;
        tcmd.opacity = inherited_opacity_local;
        tcmd.font_family = resolved_font;
        list.push_back(tcmd);
      } else if (!is_button) {
        // Draw placeholder text if present and input is empty
        std::string placeholder;
        auto ph = box->style_node->node->attributes.find("placeholder");
        if (ph != box->style_node->node->attributes.end())
          placeholder = ph->second;
        if (!placeholder.empty()) {
          DisplayCommand pcmd;
          pcmd.type = DisplayCommandType::Text;
          pcmd.rect = box->dimensions.content;
          pcmd.rect.x += 5;
          pcmd.text = placeholder;
          pcmd.color = 0x009E9E9E; // grey placeholder
          pcmd.font_size = font_size;
          pcmd.italic = true;
          pcmd.fixed = is_fixed;
          pcmd.z_index = z_index;
          pcmd.opacity = inherited_opacity_local;
          pcmd.font_family = resolved_font;
          list.push_back(pcmd);
        }
      }
    } else if (tag == "img") {
      // Real images are blitted by main.cpp's blit_images_pass after text rendering.
      float img_w = box->dimensions.content.width;
      float img_h = box->dimensions.content.height;
      // If the image is reasonably sized but has no src / can't load, show alt text
      if (img_w >= 8 && img_h >= 8) {
        auto alt_it = box->style_node->node->attributes.find("alt");
        if (alt_it != box->style_node->node->attributes.end() &&
            !alt_it->second.empty()) {
          // We emit a Text command; main.cpp will skip it if the image loaded.
          // This is done via a special sentinel: we mark bg_image_url to be
          // checked in the blit pass, but also emit the alt text so the blit
          // pass can suppress it when a real image is present.
          DisplayCommand acmd;
          acmd.type = DisplayCommandType::Text;
          acmd.rect = box->dimensions.content;
          acmd.text = alt_it->second;
          acmd.color = 0x00555555;
          acmd.font_size = std::max(10, std::min(font_size, (int)img_h - 2));
          acmd.fixed = is_fixed;
          acmd.z_index = z_index;
          acmd.opacity = inherited_opacity_local;
          // text_align left (default) — image blit in Pass 2 will overwrite if loaded
          list.push_back(acmd);
        }
      }
    }
  }

  // overflow / overflow-x / overflow-y — clip children to this box's border box
  std::string overflow_val = box->style_node->value("overflow");
  std::string overflow_x_val = box->style_node->value("overflow-x");
  std::string overflow_y_val = box->style_node->value("overflow-y");
  // overflow shorthand sets both axes
  if (overflow_x_val.empty()) overflow_x_val = overflow_val;
  if (overflow_y_val.empty()) overflow_y_val = overflow_val;
  auto is_clip_val = [](const std::string &v) {
    return v == "hidden" || v == "scroll" || v == "auto";
  };
  bool clip_x = is_clip_val(overflow_x_val);
  bool clip_y = is_clip_val(overflow_y_val);
  bool clip_children = clip_x || clip_y;
  // Only clip if box has real size (avoid false clips on zero-height containers)
  if (clip_children && box->dimensions.border_box().width < 1 &&
      box->dimensions.border_box().height < 1)
    clip_children = false;

  // visibility:hidden — discard any visuals this box just pushed for itself,
  // but still emit clip+children so a visibility:visible descendant shows up.
  if (box_hidden && list.size() > hidden_truncate_size) {
    list.resize(hidden_truncate_size);
  }

  if (clip_children) {
    DisplayCommand clip_push;
    clip_push.type = DisplayCommandType::ClipPush;
    Rect clip_rect = box->dimensions.border_box();
    // Per-axis clipping: expand the non-clipped axis to effectively not clip it
    if (!clip_x) {
      clip_rect.x = -100000.f;
      clip_rect.width = 200000.f;
    }
    if (!clip_y) {
      clip_rect.y = -100000.f;
      clip_rect.height = 200000.f;
    }
    clip_push.rect = clip_rect;
    clip_push.clip_overflow_x = clip_x;
    clip_push.clip_overflow_y = clip_y;
    clip_push.fixed = is_fixed;
    // Mark overflow:auto/scroll as scrollable with total children height
    if (overflow_y_val == "auto" || overflow_y_val == "scroll" ||
        overflow_x_val == "auto" || overflow_x_val == "scroll") {
      clip_push.clip_scrollable = true;
      float max_child_bottom = 0.f;
      for (const auto &ch : box->children) {
        if (ch) {
          float cb = ch->dimensions.border_box().y + ch->dimensions.border_box().height;
          max_child_bottom = std::max(max_child_bottom, cb);
        }
      }
      clip_push.clip_content_height = max_child_bottom - box->dimensions.content.y;
    }
    list.push_back(clip_push);
  }

  // Compute sticky parent state to pass to children
  // If this element is sticky, children inherit its sticky parameters
  bool child_parent_sticky = is_sticky || parent_sticky;
  float child_parent_sticky_top = is_sticky ? sticky_top :
                                  (parent_sticky ? parent_sticky_top : 0.f);
  float child_parent_sticky_orig = is_sticky ?
      box->dimensions.border_box().y : parent_sticky_orig_y;

  // Recursively paint children (painting back-to-front based on document order)
  for (const auto &child : box->children) {
    render_layout_box(list, child, text_color, font_size, bold,
                      underline, italic, text_align, inherited_opacity_local,
                      letter_spacing,
                      child_parent_sticky,
                      child_parent_sticky_top,
                      child_parent_sticky_orig,
                      td_line, td_color_str, td_style);
  }

  if (clip_children) {
    DisplayCommand clip_pop;
    clip_pop.type = DisplayCommandType::ClipPop;
    list.push_back(clip_pop);
  }
}

DisplayList build_display_list(const std::shared_ptr<LayoutBox> &root) {
  DisplayList list;
  if (root) {
    render_layout_box(list, root, 0x00000000, 16, false); // Default to black
  }
  return list;
}
