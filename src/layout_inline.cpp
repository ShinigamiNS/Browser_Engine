// layout_inline.cpp — Inline layout helpers
// Moved from layout.cpp (Split C)
#include "layout.h"
#include "css_values.h"
#include "font_metrics.h"
#include "style.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

/* ── css_line_height ─────────────────────────────────────────────────────── */

// Compute the CSS line-height for a box as a pixel value
float css_line_height(LayoutBox *box, int fs) {
  if (!box || !box->style_node)
    return (float)fs * 1.4f;
  auto it = box->style_node->specified_values.find("line-height");
  if (it == box->style_node->specified_values.end())
    return (float)fs * 1.4f;
  const std::string &v = it->second;
  if (v == "normal")
    return (float)fs * 1.4f;
  try {
    // bare number = multiplier
    if (!v.empty() && (isdigit((unsigned char)v[0]) || v[0] == '.') &&
        v.find("px") == std::string::npos && v.find('%') == std::string::npos) {
      return std::stof(v) * fs;
    }
    return parse_px(v, (float)fs);
  } catch (...) {
    return (float)fs * 1.4f;
  }
}

/* ── collapse_whitespace ─────────────────────────────────────────────────── */

// Collapse whitespace in a text node's data per CSS white-space:normal rules
// Collapse runs of whitespace to single space. Preserve leading/trailing
// single space (significant for inter-element spacing in inline flow).
std::string collapse_whitespace(const std::string &s) {
  if (s.empty()) return s;
  std::string out;
  out.reserve(s.size());
  bool prev_sp = false;
  for (unsigned char c : s) {
    bool sp = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (sp) {
      if (!prev_sp) {
        out += ' ';
        prev_sp = true;
      }
    } else {
      out += (char)c;
      prev_sp = false;
    }
  }
  return out;
}

/* ── tokenize_words ──────────────────────────────────────────────────────── */

// Break text into word-level tokens for line-breaking.
// Each token includes a trailing space (word separator) except the last.
// Leading spaces produce a space-only token to preserve inter-element gaps.
static std::vector<std::pair<std::string, float>>
tokenize_words(const std::string &text, int fs, bool bold,
               float letter_spacing = 0.f, bool italic = false) {
  auto &fm = FontMetrics::get_instance();
  std::vector<std::pair<std::string, float>> tokens;
  if (text.empty()) return tokens;

  size_t i = 0;
  size_t len = text.size();

  // Leading whitespace → emit a space token (inter-element gap)
  if (i < len && (text[i] == ' ' || text[i] == '\t')) {
    float w = fm.measure_text(" ", fs, bold, letter_spacing, italic).width;
    tokens.push_back({" ", w});
    while (i < len && (text[i] == ' ' || text[i] == '\t')) ++i;
  }

  // Extract words, each followed by a trailing space (except possibly last)
  while (i < len) {
    // Collect non-space chars (the word)
    size_t ws = i;
    while (i < len && text[i] != ' ' && text[i] != '\t') ++i;
    std::string word = text.substr(ws, i - ws);

    // Skip trailing spaces
    bool has_trailing = false;
    while (i < len && (text[i] == ' ' || text[i] == '\t')) { ++i; has_trailing = true; }

    // Append trailing space to word (acts as word separator in rendering)
    if (has_trailing || i < len)
      word += ' ';

    float w = fm.measure_text(word, fs, bold, letter_spacing, italic).width;
    tokens.push_back({word, w});
  }
  return tokens;
}

/* ── InlineFragment / LineBox ────────────────────────────────────────────── */

struct InlineFragment {
  LayoutBox *box = nullptr;
  float x = 0;           // left edge (absolute)
  float width = 0;       // fragment width
  float ascent = 0;      // distance from baseline to top of content
  float descent = 0;     // distance from baseline to bottom of content
  float line_height = 0; // css line-height for this fragment
  bool is_text = false;
};

struct LineBox {
  std::vector<InlineFragment> frags;
  float x_start = 0;      // left edge of line (= content.x of parent)
  float width = 0;        // available width
  float height = 0;       // final height after alignment
  float baseline = 0;     // y of baseline within the line (from top of line)
  std::string text_align; // "left", "center", "right"
};

/* ── commit_line ─────────────────────────────────────────────────────────── */

// Commit a finished LineBox: set final y positions for all fragments using
// baseline alignment, return the total line height consumed.
static float commit_line(LineBox &line, float line_y) {
  if (line.frags.empty())
    return 0.f;

  // Find max ascent and max descent across all fragments
  float max_ascent = 0.f;
  float max_descent = 0.f;
  for (auto &f : line.frags) {
    max_ascent = std::max(max_ascent, f.ascent);
    max_descent = std::max(max_descent, f.descent);
  }
  float line_height = max_ascent + max_descent;
  line.height = line_height;
  line.baseline = max_ascent;

  // Compute text-align offset: shift all fragments for center/right
  float align_offset = 0.f;
  if (!line.text_align.empty() && line.text_align != "left") {
    // Find the rightmost edge of content on this line
    float used_width = 0.f;
    for (auto &f : line.frags) {
      float right = (f.x - line.x_start) + f.width;
      used_width = std::max(used_width, right);
    }
    float slack = line.width - used_width;
    if (slack > 0.f) {
      if (line.text_align == "center")
        align_offset = slack / 2.f;
      else if (line.text_align == "right")
        align_offset = slack;
    }
  }

  // Position each fragment: baseline-align (top of content = baseline - ascent)
  for (auto &f : line.frags) {
    if (!f.box)
      continue;
    float top_offset = max_ascent - f.ascent; // push down to align baselines
    f.box->dimensions.content.x = f.x + align_offset;
    f.box->dimensions.content.y = line_y + top_offset;
  }
  return line_height;
}

/* ── layout_inline_flow ──────────────────────────────────────────────────── */

void layout_inline_flow(LayoutBox *parent) {
  auto &fm = FontMetrics::get_instance();
  int pfs = get_font_size(parent->style_node);
  float max_x = parent->dimensions.content.x + parent->dimensions.content.width;
  float line_x = parent->dimensions.content.x; // left edge of each line
  float cursor_x = line_x;
  float cursor_y = parent->dimensions.content.y;


  // Check white-space: nowrap on the parent container
  bool parent_nowrap =
      parent->style_node &&
      parent->style_node->specified_values.count("white-space") &&
      parent->style_node->specified_values.at("white-space") == "nowrap";

  // Resolve text-align from parent's style (inherited property)
  std::string text_align_val;
  if (parent->style_node) {
    text_align_val = parent->style_node->value("text-align");
  }

  // text-indent: applies to the first line of block content
  float text_indent = 0.f;
  if (parent->style_node) {
    std::string ti = parent->style_node->value("text-indent");
    if (!ti.empty() && ti != "0") {
      text_indent = parse_px(ti, parent->dimensions.content.width);
    }
  }

  LineBox current_line;
  current_line.x_start = line_x;
  current_line.width = parent->dimensions.content.width;
  current_line.text_align = text_align_val;

  // Apply text-indent to the first line
  bool first_line = true;
  if (text_indent != 0.f) {
    cursor_x += text_indent;
  }

  // Flush current line: apply baseline alignment, advance cursor_y
  auto flush_line = [&]() {
    if (current_line.frags.empty())
      return;
    // Save old positions before commit_line overwrites them.
    // Children of each fragment were laid out relative to these old positions
    // (via child->layout(cb)), so we need the delta, not the absolute value.
    struct OldPos { LayoutBox *box; float x, y; };
    std::vector<OldPos> old_positions;
    for (auto &f : current_line.frags) {
      if (f.box && !f.is_text) {
        old_positions.push_back({f.box, f.box->dimensions.content.x,
                                 f.box->dimensions.content.y});
      }
    }
    float h = commit_line(current_line, cursor_y);
    // Shift inline-element children by the delta between new and old positions
    for (auto &op : old_positions) {
      float dx = op.box->dimensions.content.x - op.x;
      float dy = op.box->dimensions.content.y - op.y;
      if (dx != 0 || dy != 0)
        offset_children(op.box, dx, dy);
    }
    cursor_y += h;
    cursor_x = line_x;
    current_line.frags.clear();
  };

  for (auto &child : parent->children) {
    if (!child)
      continue;

    bool is_text = child->style_node && child->style_node->node &&
                   child->style_node->node->type == NodeType::Text;

    // ── <br> — hard line break ────────────────────────────────────────────
    bool is_br = !is_text && child->style_node && child->style_node->node &&
                 child->style_node->node->data == "br";
    if (is_br) {
      // Add a zero-width fragment so the line gets the parent's line-height
      InlineFragment br_frag;
      br_frag.box = child.get();
      br_frag.is_text = false;
      br_frag.x = cursor_x;
      br_frag.width = 0;
      float lh = css_line_height(parent, pfs);
      br_frag.ascent = (float)pfs * 0.8f;
      br_frag.descent = lh - br_frag.ascent;
      br_frag.line_height = lh;
      current_line.frags.push_back(br_frag);
      flush_line();
      continue;
    }

    if (is_text) {
      int fs = get_font_size(child->style_node);
      bool bold = get_font_weight_bold(child->style_node);
      float lh = css_line_height(child.get(), fs);
      // Ascent ≈ 80% of font-size (typical for Latin fonts)
      float ascent = (float)fs * 0.8f;
      float descent = lh - ascent;

      // Check white-space: nowrap on the child text node's parent element
      bool child_nowrap = parent_nowrap;
      if (!child_nowrap && child->style_node &&
          child->style_node->specified_values.count("white-space") &&
          child->style_node->specified_values.at("white-space") == "nowrap")
        child_nowrap = true;

      // overflow-wrap: break-word — allows breaking inside words
      bool break_word = false;
      {
        std::string ow_val = parent->style_node
                                 ? parent->style_node->value("overflow-wrap")
                                 : "";
        if (ow_val.empty() && child->style_node)
          ow_val = child->style_node->value("overflow-wrap");
        if (ow_val == "break-word" || ow_val == "anywhere")
          break_word = true;
        // word-break: break-all also breaks at character level
        std::string wb_val =
            parent->style_node ? parent->style_node->value("word-break") : "";
        if (wb_val == "break-all")
          break_word = true;
      }

      // letter-spacing from parent
      float child_letter_spacing = 0.f;
      {
        std::string ls = parent->style_node
                             ? parent->style_node->value("letter-spacing")
                             : "";
        if (ls.empty() && child->style_node)
          ls = child->style_node->value("letter-spacing");
        if (!ls.empty() && ls != "normal") {
          try {
            if (ls.size() > 2 && ls.substr(ls.size() - 2) == "px")
              child_letter_spacing = std::stof(ls);
            else if (ls.size() > 2 && ls.substr(ls.size() - 2) == "em")
              child_letter_spacing = std::stof(ls) * fs;
            else
              child_letter_spacing = std::stof(ls);
          } catch (...) {
          }
        }
      }

      // Determine if whitespace should be preserved (white-space:
      // pre/pre-wrap/pre-line)
      bool preserve_ws =
          child_nowrap; // nowrap implies at least some preservation
      {
        std::string ws_v =
            parent->style_node ? parent->style_node->value("white-space") : "";
        if (ws_v.empty() && child->style_node)
          ws_v = child->style_node->value("white-space");
        if (ws_v == "pre" || ws_v == "pre-wrap" || ws_v == "pre-line")
          preserve_ws = true;
      }
      std::string collapsed =
          preserve_ws ? child->style_node->node->data
                      : collapse_whitespace(child->style_node->node->data);
      bool child_italic = get_font_italic(child->style_node);
      auto tokens = tokenize_words(collapsed, fs, bold, child_letter_spacing, child_italic);

      // Place this text node's box at the start of its first fragment on
      // the line.  When text wraps, store extra fragments on the LayoutBox
      // so the paint step can emit multiple display commands.
      bool placed = false;
      float frag_x_start = cursor_x;
      float frag_width = 0.f;
      std::string frag_text;
      child->extra_fragments.clear();

      for (size_t ti = 0; ti < tokens.size(); ++ti) {
        const std::string &word = tokens[ti].first;
        float w = tokens[ti].second;
        float avail_w = max_x - line_x;
        // Wrap: if word doesn't fit and we're not at line start, break
        if (!child_nowrap && cursor_x + w > max_x + 0.5f &&
            cursor_x > line_x + 0.5f) {
          if (placed) {
            InlineFragment f;
            f.box = child.get();
            f.is_text = true;
            f.x = frag_x_start;
            f.width = frag_width;
            f.ascent = ascent;
            f.descent = descent;
            f.line_height = lh;
            current_line.frags.push_back(f);
            // After commit_line sets the y, record this fragment's text
            // The fragment text will be saved after flush_line via callback
          }
          // Before flushing, save the text for the fragment we just pushed
          if (placed && !frag_text.empty()) {
            // We need to record: after flush_line commits this fragment,
            // the box position is set. We save the text now and fix up after.
            { TextFragment tf; tf.rect = Rect(frag_x_start, 0, frag_width, (float)fs); tf.text = frag_text; child->extra_fragments.push_back(tf); }
          }
          flush_line();
          // After flush_line, child->dimensions.content.y has the committed y
          // Update the last extra fragment's y
          if (!child->extra_fragments.empty()) {
            auto &ef = child->extra_fragments.back();
            ef.rect.y = child->dimensions.content.y;
          }
          frag_x_start = cursor_x;
          frag_width = 0.f;
          frag_text.clear();
          placed = false;
        }
        // break-word: if word is wider than the full available line, break it
        // by chars
        if (!child_nowrap && break_word && w > avail_w + 0.5f && avail_w > 0) {
          auto &fm2 = FontMetrics::get_instance();
          float char_x = cursor_x;
          std::string seg;
          float seg_w = 0.f;
          for (size_t ci = 0; ci < word.size();) {
            // Get one UTF-8 character
            unsigned char ch = (unsigned char)word[ci];
            int char_bytes = 1;
            if (ch >= 0xF0)
              char_bytes = 4;
            else if (ch >= 0xE0)
              char_bytes = 3;
            else if (ch >= 0xC0)
              char_bytes = 2;
            std::string ch_str = word.substr(ci, char_bytes);
            float cw = fm2.measure_text(ch_str, fs, bold).width;
            if (!seg.empty() && char_x + seg_w + cw > max_x + 0.5f) {
              // Flush segment as a fragment, wrap line
              if (!placed) {
                frag_x_start = char_x;
                placed = true;
              }
              frag_width += seg_w;
              InlineFragment sf;
              sf.box = child.get();
              sf.is_text = true;
              sf.x = frag_x_start;
              sf.width = frag_width;
              sf.ascent = ascent;
              sf.descent = descent;
              sf.line_height = lh;
              current_line.frags.push_back(sf);
              flush_line();
              frag_x_start = cursor_x;
              frag_width = 0.f;
              placed = false;
              char_x = cursor_x;
              seg.clear();
              seg_w = 0.f;
            }
            seg += ch_str;
            seg_w += cw;
            ci += char_bytes;
          }
          if (!seg.empty()) {
            if (!placed) {
              frag_x_start = char_x;
              placed = true;
            }
            frag_width += seg_w;
            cursor_x = char_x + seg_w;
          }
          continue; // skip normal word placement below
        }
        if (!placed) {
          frag_x_start = cursor_x;
          placed = true;
        }
        frag_text += word;
        frag_width += w;
        cursor_x += w;
      }

      // Commit last fragment for this text node
      {
        InlineFragment f;
        f.box = child.get();
        f.is_text = true;
        f.x = frag_x_start;
        f.width = frag_width;
        f.ascent = ascent;
        f.descent = descent;
        f.line_height = lh;
        child->dimensions.content.x = frag_x_start;
        child->dimensions.content.width = frag_width;
        child->dimensions.content.height = (float)fs;
        current_line.frags.push_back(f);
        // If we had extra fragments, add the last fragment too (don't mutate node->data)
        if (!child->extra_fragments.empty() && !frag_text.empty()) {
          { TextFragment tf; tf.rect = Rect(frag_x_start, 0, frag_width, (float)fs); tf.text = frag_text; child->extra_fragments.push_back(tf); }
        }
      }

    } else {
      // ── Inline replaced/element child ────────────────────────────────
      Dimensions cb = parent->dimensions;
      cb.content.height = 0;
      child->layout(cb);

      float w = child->dimensions.margin_box().width;
      float h = child->dimensions.margin_box().height;

      // Wrap if needed (skip if white-space: nowrap)
      if (!parent_nowrap && cursor_x + w > max_x + 0.5f &&
          cursor_x > line_x + 0.5f) {
        flush_line();
      }

      // Treat inline element as a baseline-aligned fragment
      int child_fs = get_font_size(child->style_node);
      float lh = css_line_height(child.get(), child_fs);
      InlineFragment f;
      f.box = child.get();
      f.is_text = false;
      f.x = cursor_x + child->dimensions.margin.left;
      f.width = w;
      // Use font-based ascent/descent so inline elements share baseline with text
      f.ascent = (float)child_fs * 0.8f;
      f.descent = lh - f.ascent;
      f.line_height = lh;

      // Park at (0,0); commit_line will set absolute y
      // child->dimensions.content.x = 0;
      // child->dimensions.content.y = 0;
      current_line.frags.push_back(f);
      cursor_x += w;
    }
  }

  flush_line();

  // Fix up y for any last extra_fragment that was just committed
  for (auto &child : parent->children) {
    if (child && !child->extra_fragments.empty()) {
      auto &ef = child->extra_fragments.back();
      if (ef.rect.y == 0.f) {
        ef.rect.y = child->dimensions.content.y;
      }
    }
  }

  parent->dimensions.content.height = cursor_y - parent->dimensions.content.y;
}
