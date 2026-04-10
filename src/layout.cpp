#include "layout.h"
#include "css_values.h"
#include "font_metrics.h"
#include "image_cache.h"

extern std::string g_current_page_url;
#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Debug log — disabled
static void dbg(const char *, ...) {}

// Exported debug log for layout_inline.cpp
void dbg_inline(const char *, ...) {}

static bool is_display_none(const std::shared_ptr<StyledNode> &style) {
  if (!style)
    return false;

  auto it = style->specified_values.find("display");
  if (it == style->specified_values.end())
    return false;

  return it->second == "none";
}

/* Inline element detection */

// offset_children() declared in layout.h (non-static, used by layout_inline.cpp)

static bool is_inline_tag(const std::string &tag) {

  static const std::vector<std::string> inline_tags = {
      "span",  "a",     "b",   "strong", "em",     "img",     "input",
      "label", "small", "i",   "u",      "code",   "abbr",    "cite",
      "q",     "sub",   "sup", "button", "select", "textarea"};

  for (const auto &t : inline_tags)
    if (t == tag)
      return true;

  return false;
}

static float estimate_text_width(const std::string &text) {
  return text.length() * 8.0f; // simple font estimate
}

// parse_px(), style_value() moved to css_values.cpp (Split B)

/* Layout entry */

void LayoutBox::layout(Dimensions containing_block) {
  static bool s_first = true;
  if (s_first) { dbg("[LAYOUT] layout() called — engine is running\n"); s_first = false; }
  auto &style = style_node->specified_values;
  std::string pos = style.count("position") ? style.at("position") : "static";

  // Fixed elements are positioned relative to the viewport, not the document
  // flow.
  if (pos == "fixed") {
    // For fixed elements, containing_block.width is the viewport width.
    // We need the real viewport HEIGHT — store it in content.x temporarily via
    // a convention: The caller sets viewport.content.height=0 for the root, but
    // passes real height via a separate field. We use the original
    // containing_block's content.width as a proxy for viewport width, and
    // re-use content.height=0 means we must fall back to a reasonable value.
    // Use the parent's content.width as viewport width (correct), and for
    // height use 768 default since the viewport height is not reliably passed
    // through.
    float vp_w = containing_block.content.width;
    // Use the real viewport height via extern global set in main.cpp
    extern int g_viewport_height;
    float vp_h = g_viewport_height > 0 ? (float)g_viewport_height : 768.0f;

    Dimensions viewport;
    viewport.content.x = 0;
    viewport.content.y = 0;
    viewport.content.width = vp_w;
    viewport.content.height = 0;
    calculate_block_width(viewport);

    float left = style.count("left") ? parse_px(style.at("left"), vp_w) : 0;
    float top = style.count("top") ? parse_px(style.at("top"), vp_h) : 0;
    float right = style.count("right") ? parse_px(style.at("right"), vp_w) : -1;
    float bottom =
        style.count("bottom") ? parse_px(style.at("bottom"), vp_h) : -1;
    dimensions.content.x =
        (right >= 0) ? vp_w - dimensions.content.width - right : left;
    dimensions.content.y =
        (bottom >= 0) ? vp_h - dimensions.content.height - bottom : top;
    layout_block_children();
    calculate_block_height(vp_h); // fixed: containing block is viewport
    return; // fixed elements do NOT contribute to normal flow height
  }

  // Inline elements: position is set by parent's inline flow, not
  // calculate_block_position
  bool is_inline = (box_type == BoxType::InlineNode);

  // Some tags are always treated as inline containers
  if (!is_inline && style_node && style_node->node &&
      style_node->node->type == NodeType::Element) {
    std::string tag = style_node->node->data;
    if (tag == "span" || tag == "a" || tag == "b" || tag == "strong" ||
        tag == "em" || tag == "i" || tag == "u" || tag == "label" ||
        tag == "abbr" || tag == "code" || tag == "small" || tag == "sub" ||
        tag == "sup")
      is_inline = true;
  }

  if (is_inline) {
    // Inline elements: width shrinks to content, position set by parent's flow
    // For img: use width/height attributes, then style, then default
    if (style_node->node && style_node->node->type == NodeType::Element &&
        style_node->node->data == "img") {
      float iw = 0, ih = 0;
      auto &attrs = style_node->node->attributes;
      // HTML width/height attributes as base values
      if (attrs.count("width"))
        iw = (float)atof(attrs.at("width").c_str());
      if (attrs.count("height"))
        ih = (float)atof(attrs.at("height").c_str());
      // CSS overrides, but only if they resolve to a positive value
      if (style.count("width")) {
        float css_w = parse_px(style.at("width"), containing_block.content.width);
        if (css_w > 0) iw = css_w;
      }
      if (style.count("height")) {
        float css_h = parse_px(style.at("height"), containing_block.content.height);
        if (css_h > 0) ih = css_h;
      }
      // Clamp oversized images (e.g. SVG icons with viewBox=960x960)
      // to a reasonable max based on containing block
      float max_w = containing_block.content.width;
      if (max_w > 0 && iw > max_w) {
        float scale = max_w / iw;
        iw = max_w;
        if (ih > 0) ih *= scale; // preserve aspect ratio
      }
      // Maintain aspect ratio when only one dimension is given
      if (iw > 0 && ih <= 0) ih = iw; // square fallback
      if (ih > 0 && iw <= 0) iw = ih; // square fallback
      dimensions.content.width =
          iw > 0 ? iw : 24; // default small if no size given
      dimensions.content.height = ih > 0 ? ih : 24;
      return;
    }
    // Non-img inline elements: only set width from style if explicitly given
    if (style.count("width"))
      dimensions.content.width =
          parse_px(style.at("width"), containing_block.content.width);
    if (style.count("height"))
      dimensions.content.height =
          parse_px(style.at("height"), containing_block.content.height);

    // Default sizes for form controls so they're visible without explicit CSS
    if (style_node->node && style_node->node->type == NodeType::Element) {
      const std::string &tag = style_node->node->data;
      std::string input_type;
      if (style_node->node->attributes.count("type"))
        input_type = style_node->node->attributes.at("type");

      if (dimensions.content.width < 1) {
        if (tag == "textarea") {
          dimensions.content.width = 200; // text areas get a reasonable default
        } else if (tag == "input" && input_type != "hidden" &&
                   input_type != "submit" && input_type != "button" &&
                   input_type != "checkbox" && input_type != "radio") {
          dimensions.content.width =
              200; // text inputs get a reasonable default
        } else if ((tag == "input" &&
                    (input_type == "submit" || input_type == "button")) ||
                   tag == "button") {
          // Auto-size button width to fit its text label + padding
          std::string label;
          if (tag == "input") {
            auto v = style_node->node->attributes.find("value");
            if (v != style_node->node->attributes.end()) label = v->second;
          }
          if (!label.empty()) {
            auto tm = FontMetrics::get_instance().measure_text(label, 14, false);
            dimensions.content.width = tm.width + 20; // 10px padding each side
          } else {
            dimensions.content.width = 80; // fallback
          }
        }
      }
      if (dimensions.content.height < 1 &&
          (tag == "input" || tag == "textarea" || tag == "button")) {
        if ((tag == "input" && (input_type == "submit" || input_type == "button")) ||
            tag == "button")
          dimensions.content.height = 36; // buttons need more height for text
        else
          dimensions.content.height = 20;
      }
    }
    // Apply padding
    dimensions.padding.left =
        style_value(style, "padding-left", 0, containing_block.content.width);
    dimensions.padding.right =
        style_value(style, "padding-right", 0, containing_block.content.width);
    dimensions.padding.top =
        style_value(style, "padding-top", 0, containing_block.content.width);
    dimensions.padding.bottom =
        style_value(style, "padding-bottom", 0, containing_block.content.width);
    dimensions.margin.left =
        style_value(style, "margin-left", 0, containing_block.content.width);
    dimensions.margin.right =
        style_value(style, "margin-right", 0, containing_block.content.width);
    dimensions.margin.top =
        style_value(style, "margin-top", 0, containing_block.content.width);
    dimensions.margin.bottom =
        style_value(style, "margin-bottom", 0, containing_block.content.width);
    // Layout own children (text nodes etc) at relative (0,0)
    // Parent's flush_inline_run will set absolute position afterward
    dimensions.content.x = 0;
    dimensions.content.y = 0;

    // display: inline-block and inline-flex have their own block formatting
    // context — use block children layout if they contain non-inline children.
    bool is_inline_block =
        style.count("display") && (style.at("display") == "inline-block" ||
                                   style.at("display") == "inline-flex");
    if (is_inline_block) {
      // Check if any child is non-inline (has block children)
      bool has_block_child = false;
      for (auto &ch : children) {
        if (ch && ch->box_type != BoxType::InlineNode) {
          has_block_child = true;
          break;
        }
      }
      if (has_block_child) {
        // If containing block provides positive width (e.g. from flex-grow
        // allocation), use it as the available width for children. This is
        // how a flex child like <div display:inline-block flex-grow:1>
        // propagates its allocated width to its children (e.g. Google's
        // header with justify-content:flex-end).
        bool cb_provides_width = (!style.count("width") || style.at("width").empty())
                                 && containing_block.content.width > 0;
        if (cb_provides_width) {
          dimensions.content.width = containing_block.content.width
              - dimensions.padding.left - dimensions.padding.right
              - dimensions.margin.left - dimensions.margin.right;
          if (dimensions.content.width < 0) dimensions.content.width = 0;
        }
        layout_block_children();
        // Shrink-wrap width to content ONLY if containing block didn't
        // provide a width (pure shrink-to-fit mode).
        if (!cb_provides_width) {
          if (!style.count("width") || style.at("width").empty()) {
            float max_child_w = 0;
            for (auto &ch : children) {
              if (ch)
                max_child_w =
                    std::max(max_child_w, ch->dimensions.margin_box().width);
            }
            if (max_child_w > 0)
              dimensions.content.width = max_child_w;
          }
        }
        calculate_block_height(0);
        return;
      }
    }

    layout_inline_children();
    return;
  } else if (box_type == BoxType::BlockNode) {
    calculate_block_width(containing_block);
    calculate_block_position(containing_block);
    layout_block_children();
    calculate_block_height(containing_block.content.height);
  } else if (box_type == BoxType::FlexContainer) {
    calculate_block_width(containing_block);
    calculate_block_position(containing_block);

    bool is_column =
        style.count("flex-direction") && style.at("flex-direction") == "column";
    std::string justify = style.count("justify-content")
                              ? style.at("justify-content")
                              : "flex-start";
    std::string align_items =
        style.count("align-items") ? style.at("align-items") : "stretch";

    bool flex_wrap =
        style.count("flex-wrap") && (style.at("flex-wrap") == "wrap" ||
                                     style.at("flex-wrap") == "wrap-reverse");

    // Parse gap (column-gap for row direction, row-gap for column direction)
    float gap_size = 0;
    if (!is_column) {
      if (style.count("column-gap"))
        gap_size = parse_px(style.at("column-gap"), dimensions.content.width);
      else if (style.count("gap"))
        gap_size = parse_px(style.at("gap"), dimensions.content.width);
    } else {
      if (style.count("row-gap"))
        gap_size = parse_px(style.at("row-gap"), dimensions.content.width);
      else if (style.count("gap"))
        gap_size = parse_px(style.at("gap"), dimensions.content.width);
    }

    // Helper: read flex-grow value from a child's style
    auto get_flex_grow = [](const std::shared_ptr<LayoutBox> &c) -> float {
      if (!c || !c->style_node)
        return 0;
      auto &cs = c->style_node->specified_values;
      if (cs.count("flex-grow")) {
        try {
          return std::stof(cs.at("flex-grow"));
        } catch (...) {
        }
      }
      // flex: N shorthand also sets flex-grow
      if (cs.count("flex")) {
        const auto &fv = cs.at("flex");
        if (fv != "none" && fv != "auto") {
          try {
            return std::stof(fv);
          } catch (...) {
          }
        }
      }
      return 0;
    };

    // First pass: layout each child with a shrink-to-fit containing block.
    // Using width=0 prevents block children from blindly stretching to the
    // container width — they will size themselves from their content instead.
    // Children with explicit CSS width or height:100% will still resolve
    // correctly because parse_px uses the containing-block reference only for
    // percentage values that reference width.
    // Helper: check if a child is out-of-flow (position:absolute/fixed)
    auto is_out_of_flow = [](const std::shared_ptr<LayoutBox> &c) -> bool {
      if (!c || !c->style_node)
        return false;
      auto &cs = c->style_node->specified_values;
      if (!cs.count("position"))
        return false;
      const auto &p = cs.at("position");
      return p == "absolute" || p == "fixed";
    };

    Dimensions size_cb_shrink = dimensions;
    size_cb_shrink.content.height = 0;
    if (!is_column) {
      size_cb_shrink.content.width = 0; // row flex: shrink-to-fit width
    }
    // Column flex with align-items != stretch: children should shrink-to-fit
    // on the cross axis instead of stretching to container width.
    if (is_column && align_items != "stretch") {
      size_cb_shrink.content.width = 0;
    }
    for (auto &child : children) {
      if (!child)
        continue;
      if (is_out_of_flow(child))
        continue; // skip absolutely-positioned items
      // If child has an explicit CSS width, layout with full container as ref
      bool has_explicit_w =
          child->style_node &&
          child->style_node->specified_values.count("width") &&
          !child->style_node->specified_values.at("width").empty();
      if (has_explicit_w) {
        Dimensions ref_cb = dimensions;
        ref_cb.content.height = 0;
        child->layout(ref_cb);
      } else {
        child->layout(size_cb_shrink);
        // After shrink-to-fit: for flex/block containers that have children,
        // compute natural width from children so they can be sized properly
        // when allocated space in the grow/fallback pass.
        if (child->dimensions.content.width < 1 &&
            (child->box_type == BoxType::FlexContainer ||
             child->box_type == BoxType::BlockNode)) {
          float natural_w = 0;
          for (auto &gc : child->children) {
            if (!gc)
              continue;
            float cw = gc->dimensions.margin_box().width;
            if (child->box_type == BoxType::FlexContainer)
              natural_w += cw; // flex: children are side by side
            else
              natural_w = std::max(natural_w, cw); // block: children stack
          }
          if (natural_w > 0)
            child->dimensions.content.width = natural_w;
        }
      }
    }

    // Collect total natural size and find flex-grow items
    float total_fixed_size = 0;
    float flex_grow_total = 0;
    for (auto &child : children) {
      if (!child || is_out_of_flow(child))
        continue;
      float fg = get_flex_grow(child);
      if (fg == 0) {
        total_fixed_size += is_column ? child->dimensions.margin_box().height
                                      : child->dimensions.margin_box().width;
      }
      flex_grow_total += fg;
    }

    float main_container_size =
        is_column ? dimensions.content.height : dimensions.content.width;

    // Count in-flow children (needed for gap calculation)
    int in_flow_count = 0;
    for (auto &child : children)
      if (child && !is_out_of_flow(child))
        in_flow_count++;
    float total_gap = gap_size * std::max(0, in_flow_count - 1);

    if (main_container_size == 0) {
      // No explicit container size: size from content
      float total_all = 0;
      for (auto &child : children) {
        if (!child)
          continue;
        total_all += is_column ? child->dimensions.margin_box().height
                               : child->dimensions.margin_box().width;
      }
      main_container_size = total_all + total_gap;
    }

    // Distribute remaining space to flex-grow children
    float remaining = main_container_size - total_fixed_size - total_gap;

    if (flex_grow_total > 0 && remaining > 0) {
      for (auto &child : children) {
        if (!child || is_out_of_flow(child))
          continue;
        float fg = get_flex_grow(child);
        if (fg > 0) {
          float new_main = remaining * fg / flex_grow_total;
          Dimensions grow_cb = dimensions;
          grow_cb.content.height = 0;
          if (is_column) {
            grow_cb.content.height = new_main;
          } else {
            grow_cb.content.width = new_main;
          }
          child->layout(grow_cb);
        }
      }
    } else if (flex_grow_total == 0 && remaining < -0.5f) {
      // flex-shrink: items overflow, shrink them proportionally
      float overflow = -remaining; // positive overflow amount
      // Helper to get flex-shrink value (default 1)
      auto get_flex_shrink = [](const std::shared_ptr<LayoutBox> &c) -> float {
        if (!c || !c->style_node)
          return 1;
        auto &cs = c->style_node->specified_values;
        if (cs.count("flex-shrink")) {
          try {
            return std::stof(cs.at("flex-shrink"));
          } catch (...) {
          }
        }
        return 1; // default flex-shrink is 1
      };
      float shrink_denom = 0;
      for (auto &child : children) {
        if (!child || is_out_of_flow(child))
          continue;
        float fs_val = get_flex_shrink(child);
        if (fs_val <= 0)
          continue;
        float item_size = is_column ? child->dimensions.margin_box().height
                                    : child->dimensions.margin_box().width;
        shrink_denom += fs_val * item_size;
      }
      if (shrink_denom > 0) {
        for (auto &child : children) {
          if (!child || is_out_of_flow(child))
            continue;
          float fs_val = get_flex_shrink(child);
          if (fs_val <= 0)
            continue;
          float item_size = is_column ? child->dimensions.margin_box().height
                                      : child->dimensions.margin_box().width;
          float shrink_ratio = (fs_val * item_size) / shrink_denom;
          float new_size = item_size - overflow * shrink_ratio;
          if (new_size < 0)
            new_size = 0;
          Dimensions shrink_cb = dimensions;
          shrink_cb.content.height = 0;
          if (is_column)
            shrink_cb.content.height = new_size;
          else
            shrink_cb.content.width = new_size;
          child->layout(shrink_cb);
        }
      }
    } else if (!is_column && flex_grow_total == 0 && remaining > 1) {
      // No explicit flex-grow but there is remaining horizontal space.
      // Use content.width (not margin_box) to find truly zero-width items.
      // Prefer BLOCK-type (content areas like textarea wrappers) over FLEX-type
      // (icon buttons). Give the target remaining + its own margin_box so
      // that the total layout fills the container exactly.
      std::shared_ptr<LayoutBox> target;
      for (auto &child : children) {
        if (!child || is_out_of_flow(child))
          continue;
        if (child->dimensions.content.width >= 1)
          continue; // has content already
        if (child->box_type == BoxType::BlockNode) {
          target = child;
          break; // BLOCK item preferred
        }
        if (!target)
          target = child; // first zero-content fallback
      }
      if (target) {
        float old_margin = is_column ? target->dimensions.margin_box().height
                                     : target->dimensions.margin_box().width;
        Dimensions grow_cb = dimensions;
        grow_cb.content.height = 0;
        if (is_column)
          grow_cb.content.height = remaining + old_margin;
        else
          grow_cb.content.width = remaining + old_margin;
        target->layout(grow_cb);
      }
    }

    // Recompute totals after flex-grow/shrink
    float total_main_size = 0;
    float max_cross_size = 0;
    for (auto &child : children) {
      if (!child || is_out_of_flow(child))
        continue;
      if (is_column) {
        total_main_size += child->dimensions.margin_box().height;
        max_cross_size =
            std::max(max_cross_size, child->dimensions.margin_box().width);
      } else {
        total_main_size += child->dimensions.margin_box().width;
        max_cross_size =
            std::max(max_cross_size, child->dimensions.margin_box().height);
      }
    }
    total_main_size += total_gap;

    float cursor_main = is_column ? dimensions.content.y : dimensions.content.x;
    // Re-read container size from dimensions in case grow/shrink pass updated it
    {
      float dims_main = is_column ? dimensions.content.height : dimensions.content.width;
      if (dims_main > main_container_size)
        main_container_size = dims_main;
    }
    if (main_container_size == 0)
      main_container_size = total_main_size;

    // Auto-margin handling: margin:auto in a flex container absorbs free space.
    // This is how Google pushes nav items (Sign In, Apps grid) to the right side
    // of the header bar. Detect all auto margins, split free space equally.
    float auto_margin_each = 0;
    bool has_any_auto_margin = false;
    if (!is_column) {
      int n_auto = 0;
      for (auto &child : children) {
        if (!child || is_out_of_flow(child)) continue;
        if (!child->style_node) continue;
        auto &cs = child->style_node->specified_values;
        if (cs.count("margin-left") && cs.at("margin-left") == "auto") n_auto++;
        if (cs.count("margin-right") && cs.at("margin-right") == "auto") n_auto++;
      }
      if (n_auto > 0) {
        float free = std::max(0.0f, main_container_size - total_main_size);
        auto_margin_each = free / (float)n_auto;
        has_any_auto_margin = true;
      }
    }

    // When auto-margins are present, justify-content is overridden — items are
    // placed from the start; auto margins absorb all free space instead.
    float cursor_main_start = cursor_main;
    if (!has_any_auto_margin) {
      if (justify == "center" && main_container_size > total_main_size)
        cursor_main += (main_container_size - total_main_size) / 2.0f;
      else if (justify == "flex-end" && main_container_size > total_main_size)
        cursor_main += main_container_size - total_main_size;
    }
    (void)cursor_main_start;

    float spacing = 0;
    if (!has_any_auto_margin && main_container_size > total_main_size) {
      float free = main_container_size - total_main_size;
      if (justify == "space-between" && in_flow_count > 1)
        spacing = free / ((float)in_flow_count - 1);
      else if (justify == "space-evenly" && in_flow_count > 0) {
        spacing = free / ((float)in_flow_count + 1);
        cursor_main += spacing; // space before first item
      } else if (justify == "space-around" && in_flow_count > 0) {
        spacing = free / (float)in_flow_count;
        cursor_main += spacing / 2.0f; // half-space before first item
      }
    }
    // Cross-axis container size for align-items
    float cross_container =
        is_column ? dimensions.content.width : dimensions.content.height;
    if (cross_container == 0)
      cross_container = max_cross_size;

    // Position pass: set final positions (skip out-of-flow)

    for (auto &child : children) {
      if (!child)
        continue;
      if (is_out_of_flow(child)) {
        // Absolutely positioned: lay out relative to this container
        Dimensions abs_cb = dimensions;
        child->layout(abs_cb);
        continue;
      }
      float old_x = child->dimensions.content.x;
      float old_y = child->dimensions.content.y;
      float new_x, new_y;

      if (is_column) {
        // Main axis = Y, cross axis = X
        float child_cross = child->dimensions.margin_box().width;
        if (align_items == "center")
          new_x = dimensions.content.x + child->dimensions.margin.left +
                  (cross_container - child_cross) / 2.0f;
        else if (align_items == "flex-end")
          new_x = dimensions.content.x + cross_container - child_cross +
                  child->dimensions.margin.left;
        else
          new_x = dimensions.content.x + child->dimensions.margin.left;
        new_y = cursor_main + child->dimensions.margin.top;
        cursor_main +=
            child->dimensions.margin_box().height + spacing + gap_size;
      } else {
        // Main axis = X, cross axis = Y
        // Handle margin-left:auto — advance cursor before placing item
        bool ml_auto = child->style_node &&
            child->style_node->specified_values.count("margin-left") &&
            child->style_node->specified_values.at("margin-left") == "auto";
        bool mr_auto = child->style_node &&
            child->style_node->specified_values.count("margin-right") &&
            child->style_node->specified_values.at("margin-right") == "auto";
        if (ml_auto) cursor_main += auto_margin_each;
        float child_cross = child->dimensions.margin_box().height;
        new_x = cursor_main + child->dimensions.margin.left; // margin.left=0 for auto
        if (align_items == "center")
          new_y = dimensions.content.y + child->dimensions.margin.top +
                  (cross_container - child_cross) / 2.0f;
        else if (align_items == "flex-end")
          new_y = dimensions.content.y + cross_container - child_cross +
                  child->dimensions.margin.top;
        else
          new_y = dimensions.content.y + child->dimensions.margin.top;
        cursor_main +=
            child->dimensions.margin_box().width + spacing + gap_size;
        if (mr_auto) cursor_main += auto_margin_each;
      }
      child->dimensions.content.x = new_x;
      child->dimensions.content.y = new_y;
      float dx = new_x - old_x;
      float dy = new_y - old_y;
      if (dx != 0 || dy != 0)
        offset_children(child.get(), dx, dy);
    }

    if (is_column)
      dimensions.content.height = total_main_size;
    else
      dimensions.content.height = std::max(max_cross_size, cross_container);

    // flex-wrap: wrap — if any items exceeded container right edge, wrap them.
    // First check whether wrapping is actually needed; if all items fit on one
    // line, preserve the positions from the justify-content pass above.
    if (flex_wrap && !is_column && dimensions.content.width > 0) {
      float container_right = dimensions.content.x + dimensions.content.width;
      // Check if any wrapping is needed
      bool needs_wrap = false;
      {
        float cx = dimensions.content.x;
        for (auto &child : children) {
          if (!child || is_out_of_flow(child)) continue;
          float cw = child->dimensions.margin_box().width;
          if (cx + cw > container_right + 0.5f &&
              cx > dimensions.content.x + 0.5f) {
            needs_wrap = true;
            break;
          }
          cx += cw + gap_size;
        }
      }
      if (needs_wrap) {
        // Re-layout with wrapping — positions are reset from the left edge
        float row_y = dimensions.content.y;
        float row_h = 0;
        float cx = dimensions.content.x;
        float total_h = 0;
        for (auto &child : children) {
          if (!child || is_out_of_flow(child))
            continue;
          float cw = child->dimensions.margin_box().width;
          float ch = child->dimensions.margin_box().height;
          if (cx + cw > container_right + 0.5f &&
              cx > dimensions.content.x + 0.5f) {
            // Wrap to next line
            total_h += row_h + gap_size;
            row_y = dimensions.content.y + total_h;
            cx = dimensions.content.x;
            row_h = 0;
          }
          float old_x = child->dimensions.content.x;
          float old_y = child->dimensions.content.y;
          float new_x = cx + child->dimensions.margin.left;
          float new_y = row_y + child->dimensions.margin.top;
          child->dimensions.content.x = new_x;
          child->dimensions.content.y = new_y;
          float dx = new_x - old_x, dy = new_y - old_y;
          if (dx != 0 || dy != 0)
            offset_children(child.get(), dx, dy);
          row_h = std::max(row_h, ch);
          cx += cw + gap_size;
        }
        dimensions.content.height = total_h + row_h;
      }
    }

    calculate_block_height(containing_block.content.height);

  } else if (box_type == BoxType::GridContainer) {
    calculate_block_width(containing_block);
    calculate_block_position(containing_block);

    // Parse grid-template-columns into column widths
    std::vector<float> col_widths;
    {
      std::string gtc = style.count("grid-template-columns")
                            ? style.at("grid-template-columns")
                            : "";
      // Expand repeat(n, ...) first
      while (true) {
        size_t rp = gtc.find("repeat(");
        if (rp == std::string::npos)
          break;
        size_t pe = gtc.find(')', rp);
        if (pe == std::string::npos)
          break;
        std::string inner = gtc.substr(rp + 7, pe - rp - 7);
        size_t comma = inner.find(',');
        if (comma == std::string::npos)
          break;
        int n = 0;
        try {
          n = std::stoi(inner.substr(0, comma));
        } catch (...) {
          n = 1;
        }
        std::string track = inner.substr(comma + 1);
        while (!track.empty() && track[0] == ' ')
          track.erase(0, 1);
        std::string expanded;
        for (int ni = 0; ni < n; ni++) {
          if (!expanded.empty())
            expanded += " ";
          expanded += track;
        }
        gtc = gtc.substr(0, rp) + expanded + gtc.substr(pe + 1);
      }
      // Parse individual track values
      std::istringstream gss(gtc);
      std::string tok;
      float total_fr = 0;
      float used_fixed = 0;
      std::vector<std::string> raw_tracks;
      while (gss >> tok)
        raw_tracks.push_back(tok);
      // First pass: fixed tracks
      for (const auto &t : raw_tracks) {
        if (!t.empty() && t.back() == '%') {
          float pct = 0;
          try {
            pct = std::stof(t.substr(0, t.size() - 1));
          } catch (...) {
          }
          col_widths.push_back(dimensions.content.width * pct / 100.f);
          used_fixed += col_widths.back();
        } else if (t.size() > 2 && t.substr(t.size() - 2) == "fr") {
          float fr = 0;
          try {
            fr = std::stof(t);
          } catch (...) {
            fr = 1;
          }
          total_fr += fr;
          col_widths.push_back(-fr); // negative = fr unit, resolved below
        } else if (t == "auto" || t.empty()) {
          col_widths.push_back(-1);
          total_fr += 1; // treat auto as 1fr
        } else {
          float w = parse_px(t, dimensions.content.width);
          col_widths.push_back(w);
          used_fixed += w;
        }
      }
      // Second pass: resolve fr units
      float fr_space = std::max(0.f, dimensions.content.width - used_fixed);
      for (auto &w : col_widths) {
        if (w < 0) {
          float fr = -w;
          w = total_fr > 0 ? fr * fr_space / total_fr : fr_space;
        }
      }
    }
    int cols = (int)col_widths.size();
    if (cols == 0) {
      cols = 1;
      col_widths.push_back(dimensions.content.width);
    }

    // gap / column-gap / row-gap
    float col_gap = 0, row_gap = 0;
    if (style.count("gap"))
      col_gap = row_gap = parse_px(style.at("gap"), dimensions.content.width);
    if (style.count("column-gap"))
      col_gap = parse_px(style.at("column-gap"), dimensions.content.width);
    if (style.count("row-gap"))
      row_gap = parse_px(style.at("row-gap"), dimensions.content.height);

    float cursor_x = dimensions.content.x;
    float cursor_y = dimensions.content.y;
    float curr_row_height = 0;
    int col_idx = 0;

    for (auto &child : children) {
      if (!child)
        continue;
      float cw = col_widths[col_idx];
      Dimensions child_cb = dimensions;
      child_cb.content.x = cursor_x + (float)col_idx * (cw + col_gap);
      child_cb.content.width = cw;
      child_cb.content.height = 0;
      child->layout(child_cb);
      child->dimensions.content.x =
          child_cb.content.x + child->dimensions.margin.left;
      child->dimensions.content.y = cursor_y + child->dimensions.margin.top;

      curr_row_height =
          std::max(curr_row_height, child->dimensions.margin_box().height);
      col_idx++;
      if (col_idx >= cols) {
        col_idx = 0;
        cursor_y += curr_row_height + row_gap;
        curr_row_height = 0;
      }
    }
    dimensions.content.height =
        (cursor_y - dimensions.content.y) + curr_row_height;

  } else if (box_type == BoxType::TableContainer) {
    // ── CSS Table Layout ──────────────────────────────────────────────────
    calculate_block_width(containing_block);
    calculate_block_position(containing_block);

    // Parse border-spacing
    float border_spacing = 2.f;
    if (style.count("border-spacing")) {
      try { border_spacing = std::stof(style.at("border-spacing")); } catch (...) {}
    }
    bool border_collapse = style.count("border-collapse") &&
                           style.at("border-collapse") == "collapse";
    float spacing = border_collapse ? 0.f : border_spacing;

    // Collect all rows and cells into a grid structure.
    // Walk children: table -> thead/tbody/tfoot/tr -> td/th
    // Row groups (thead/tbody/tfoot) are transparent — we flatten their tr children.
    struct CellInfo {
      std::shared_ptr<LayoutBox> box;
      int colspan;
      int rowspan;
      float min_w;  // intrinsic minimum width
    };
    struct RowInfo {
      std::vector<CellInfo> cells;
    };
    std::vector<RowInfo> rows;
    std::shared_ptr<LayoutBox> caption_box;

    // Helper to collect rows from a parent (table or row-group)
    auto collect_rows = [&](const std::vector<std::shared_ptr<LayoutBox>> &kids) {
      for (auto &child : kids) {
        if (!child || !child->style_node || !child->style_node->node) continue;
        std::string ctag = child->style_node->node->data;
        std::string cdisp = child->style_node->value("display");
        if (ctag == "caption" || cdisp == "table-caption") {
          caption_box = child;
          continue;
        }
        if (ctag == "thead" || ctag == "tbody" || ctag == "tfoot" ||
            cdisp == "table-row-group" || cdisp == "table-header-group" ||
            cdisp == "table-footer-group") {
          // Row group: recurse into its children for <tr>s
          for (auto &gc : child->children) {
            if (!gc || !gc->style_node || !gc->style_node->node) continue;
            std::string gtag = gc->style_node->node->data;
            std::string gdisp = gc->style_node->value("display");
            if (gtag == "tr" || gdisp == "table-row") {
              RowInfo row;
              for (auto &cell : gc->children) {
                if (!cell || !cell->style_node || !cell->style_node->node) continue;
                std::string cell_tag = cell->style_node->node->data;
                std::string cell_disp = cell->style_node->value("display");
                if (cell_tag == "td" || cell_tag == "th" || cell_disp == "table-cell") {
                  int cs = 1, rs = 1;
                  auto &attrs = cell->style_node->node->attributes;
                  if (attrs.count("colspan")) {
                    try { cs = std::stoi(attrs.at("colspan")); } catch (...) {}
                    if (cs < 1) cs = 1;
                  }
                  if (attrs.count("rowspan")) {
                    try { rs = std::stoi(attrs.at("rowspan")); } catch (...) {}
                    if (rs < 1) rs = 1;
                  }
                  row.cells.push_back({cell, cs, rs, 0.f});
                }
              }
              rows.push_back(row);
            }
          }
        } else if (ctag == "tr" || cdisp == "table-row") {
          RowInfo row;
          for (auto &cell : child->children) {
            if (!cell || !cell->style_node || !cell->style_node->node) continue;
            std::string cell_tag = cell->style_node->node->data;
            std::string cell_disp = cell->style_node->value("display");
            if (cell_tag == "td" || cell_tag == "th" || cell_disp == "table-cell") {
              int cs = 1, rs = 1;
              auto &attrs = cell->style_node->node->attributes;
              if (attrs.count("colspan")) {
                try { cs = std::stoi(attrs.at("colspan")); } catch (...) {}
                if (cs < 1) cs = 1;
              }
              if (attrs.count("rowspan")) {
                try { rs = std::stoi(attrs.at("rowspan")); } catch (...) {}
                if (rs < 1) rs = 1;
              }
              row.cells.push_back({cell, cs, rs, 0.f});
            }
          }
          rows.push_back(row);
        }
      }
    };
    collect_rows(children);

    // Determine number of columns
    int num_cols = 0;
    for (auto &row : rows) {
      int cols_in_row = 0;
      for (auto &cell : row.cells) cols_in_row += cell.colspan;
      if (cols_in_row > num_cols) num_cols = cols_in_row;
    }
    if (num_cols < 1) num_cols = 1;

    // Pass 1: Determine column widths
    // First, get intrinsic min widths by laying out each cell with 1px width
    // Then distribute available space
    std::vector<float> col_widths(num_cols, 0.f);

    // Check for explicit column widths from CSS
    // First pass: measure intrinsic widths of cells
    for (auto &row : rows) {
      int col_idx = 0;
      for (auto &cell : row.cells) {
        if (col_idx >= num_cols) break;
        // Check if cell has explicit width
        std::string cell_w_str = cell.box->style_node->value("width");
        float cell_explicit_w = 0;
        if (!cell_w_str.empty() && cell_w_str != "auto") {
          cell_explicit_w = parse_px(cell_w_str, dimensions.content.width);
        }

        if (cell.colspan == 1) {
          if (cell_explicit_w > 0) {
            col_widths[col_idx] = std::max(col_widths[col_idx], cell_explicit_w);
          } else {
            // Lay out cell with shrink-to-fit to get intrinsic width
            Dimensions probe_cb;
            probe_cb.content.width = dimensions.content.width;
            probe_cb.content.height = 0;
            cell.box->layout(probe_cb);
            float intrinsic = cell.box->dimensions.content.width +
                              cell.box->dimensions.padding.left +
                              cell.box->dimensions.padding.right;
            cell.min_w = intrinsic;
            col_widths[col_idx] = std::max(col_widths[col_idx],
                                           std::min(intrinsic, dimensions.content.width / num_cols));
          }
        }
        col_idx += cell.colspan;
      }
    }

    // Distribute remaining space: if total < available, expand proportionally
    float total_col_w = 0;
    for (int c = 0; c < num_cols; c++) {
      if (col_widths[c] < 1.f) col_widths[c] = 20.f; // minimum fallback
      total_col_w += col_widths[c];
    }
    float avail = dimensions.content.width - spacing * (num_cols + 1);
    if (avail < 0) avail = 0;
    dbg("[TABLE] %d cols, total_col_w=%.0f avail=%.0f container_w=%.0f spacing=%.0f\n",
            num_cols, total_col_w, avail, dimensions.content.width, spacing);
    for (int c = 0; c < num_cols; c++)
      dbg("[TABLE]   col[%d] = %.0f\n", c, col_widths[c]);
    if (total_col_w < avail) {
      // Distribute extra space equally
      float extra = avail - total_col_w;
      float per_col = extra / num_cols;
      for (int c = 0; c < num_cols; c++)
        col_widths[c] += per_col;
    } else if (total_col_w > avail && avail > 0) {
      // Shrink columns proportionally
      float scale = avail / total_col_w;
      for (int c = 0; c < num_cols; c++)
        col_widths[c] *= scale;
    }
    dbg("[TABLE] after distribute:\n");
    for (int c = 0; c < num_cols; c++)
      dbg("[TABLE]   col[%d] = %.0f\n", c, col_widths[c]);

    // Pass 2: Layout caption if present
    float table_y = dimensions.content.y;
    if (caption_box) {
      Dimensions cap_cb = dimensions;
      cap_cb.content.height = 0;
      caption_box->layout(cap_cb);
      caption_box->dimensions.content.x = dimensions.content.x;
      caption_box->dimensions.content.y = table_y;
      offset_children(caption_box.get(),
                      caption_box->dimensions.content.x - dimensions.content.x,
                      0);
      table_y += caption_box->dimensions.margin_box().height;
    }

    // Pass 3: Layout each row with synchronized column widths
    // Build a 2D occupied grid for rowspan tracking
    int num_rows = (int)rows.size();
    // occupied[r][c] = true if cell at (r,c) is spanned by a rowspan from above
    std::vector<std::vector<bool>> occupied(num_rows,
                                            std::vector<bool>(num_cols, false));

    std::vector<float> row_heights(num_rows, 0.f);
    float cursor_y_tbl = table_y + spacing;

    for (int r = 0; r < num_rows; r++) {
      int cell_idx = 0;
      int col = 0;
      float row_x = dimensions.content.x + spacing;

      for (int c = 0; c < num_cols && cell_idx < (int)rows[r].cells.size(); c++) {
        // Skip occupied columns (from rowspan above)
        while (col < num_cols && occupied[r][col]) {
          row_x += col_widths[col] + spacing;
          col++;
        }
        if (col >= num_cols) break;

        auto &cell = rows[r].cells[cell_idx];
        cell_idx++;

        // Calculate cell width (sum of spanned columns + spacing between them)
        float cell_w = 0;
        for (int sc = 0; sc < cell.colspan && (col + sc) < num_cols; sc++) {
          cell_w += col_widths[col + sc];
          if (sc > 0) cell_w += spacing;
        }

        // Mark occupied cells for rowspan
        for (int sr = 1; sr < cell.rowspan && (r + sr) < num_rows; sr++) {
          for (int sc = 0; sc < cell.colspan && (col + sc) < num_cols; sc++) {
            occupied[r + sr][col + sc] = true;
          }
        }

        // For border-collapse, suppress inner borders by overriding style
        // before layout so dimensions.border is computed correctly
        if (border_collapse && cell.box->style_node) {
          auto &sv = cell.box->style_node->specified_values;
          if (r > 0 || (col < num_cols && occupied[r][col])) {
            // Not first row — remove top border
            sv["border-top"] = "none";
            sv["border-top-width"] = "0";
          }
          if (col > 0) {
            // Not first column — remove left border
            sv["border-left"] = "none";
            sv["border-left-width"] = "0";
          }
        }

        // Layout cell with the computed width
        Dimensions cell_cb;
        cell_cb.content.x = row_x;
        cell_cb.content.y = cursor_y_tbl;
        cell_cb.content.width = cell_w;
        cell_cb.content.height = 0;
        cell.box->layout(cell_cb);

        // Reposition cell to exact position (accounting for border + padding)
        float target_x = row_x + cell.box->dimensions.border.left +
                         cell.box->dimensions.padding.left;
        float target_y = cursor_y_tbl + cell.box->dimensions.border.top +
                         cell.box->dimensions.padding.top;
        float dx = target_x - cell.box->dimensions.content.x;
        float dy = target_y - cell.box->dimensions.content.y;
        cell.box->dimensions.content.x = target_x;
        cell.box->dimensions.content.y = target_y;
        cell.box->dimensions.content.width = cell_w -
            cell.box->dimensions.border.left - cell.box->dimensions.border.right -
            cell.box->dimensions.padding.left - cell.box->dimensions.padding.right;
        if (cell.box->dimensions.content.width < 0)
          cell.box->dimensions.content.width = 0;
        if (dx != 0.f || dy != 0.f)
          offset_children(cell.box.get(), dx, dy);

        float cell_h = cell.box->dimensions.margin_box().height;
        if (cell.rowspan == 1) {
          row_heights[r] = std::max(row_heights[r], cell_h);
        }

        row_x += cell_w + spacing;
        col += cell.colspan;
      }

      // Ensure minimum row height
      if (row_heights[r] < 1.f) row_heights[r] = 20.f;
      cursor_y_tbl += row_heights[r] + spacing;
    }

    // Pass 4: Vertical alignment within cells
    for (int r = 0; r < num_rows; r++) {
      int cell_idx = 0;
      int col = 0;
      for (int c = 0; c < num_cols && cell_idx < (int)rows[r].cells.size(); c++) {
        while (col < num_cols && occupied[r][col]) col++;
        if (col >= num_cols) break;
        auto &cell = rows[r].cells[cell_idx];
        cell_idx++;

        float cell_h = cell.box->dimensions.margin_box().height;
        float target_h = row_heights[r];
        // For rowspan, sum spanned row heights
        for (int sr = 1; sr < cell.rowspan && (r + sr) < num_rows; sr++)
          target_h += row_heights[r + sr] + spacing;

        std::string va = cell.box->style_node->value("vertical-align");
        float dy = 0;
        if (va == "middle" || va.empty()) {
          dy = (target_h - cell_h) / 2.f;
        } else if (va == "bottom") {
          dy = target_h - cell_h;
        }
        // top: dy = 0 (default)
        if (dy > 0.5f) {
          cell.box->dimensions.content.y += dy;
          offset_children(cell.box.get(), 0, dy);
        }
        col += cell.colspan;
      }
    }

    dimensions.content.height = cursor_y_tbl - dimensions.content.y;
  }

  // Handle relative/absolute positioning offsets
  // sticky: treat like relative for static layout (no scroll awareness)
  if (pos == "relative" || pos == "sticky") {
    dimensions.content.x +=
        style_value(style, "left", 0, containing_block.content.width);
    dimensions.content.x -=
        style_value(style, "right", 0, containing_block.content.width);
    dimensions.content.y +=
        style_value(style, "top", 0, containing_block.content.height);
    dimensions.content.y -=
        style_value(style, "bottom", 0, containing_block.content.height);
  } else if (pos == "absolute") {
    dimensions.content.x =
        containing_block.content.x +
        style_value(style, "left", 0, containing_block.content.width);
    dimensions.content.y =
        containing_block.content.y +
        style_value(style, "top", 0, containing_block.content.height);
    if (style.count("right"))
      dimensions.content.x =
          containing_block.content.x + containing_block.content.width -
          dimensions.content.width -
          style_value(style, "right", 0, containing_block.content.width);
    if (style.count("bottom"))
      dimensions.content.y =
          containing_block.content.y + containing_block.content.height -
          dimensions.content.height -
          style_value(style, "bottom", 0, containing_block.content.height);
  }

  // CSS transform: translate(X, Y) — apply after normal layout
  if (style.count("transform")) {
    const std::string &tr = style.at("transform");
    // Handle translate(X) and translate(X, Y) and translateX/translateY
    auto apply_translate = [&](const std::string &inner) {
      float tx = 0, ty = 0;
      size_t comma = inner.find(',');
      if (comma != std::string::npos) {
        tx = parse_px(inner.substr(0, comma), containing_block.content.width);
        ty = parse_px(inner.substr(comma + 1), containing_block.content.height);
      } else {
        tx = parse_px(inner, containing_block.content.width);
      }
      dimensions.content.x += tx;
      dimensions.content.y += ty;
      offset_children(this, tx, ty);
    };

    // Find translate(...)
    size_t tp = tr.find("translate(");
    if (tp != std::string::npos) {
      size_t s = tp + 10, e = tr.find(')', s);
      if (e != std::string::npos)
        apply_translate(tr.substr(s, e - s));
    }
    // translateX(...)
    size_t tx_p = tr.find("translateX(");
    if (tx_p != std::string::npos) {
      size_t s = tx_p + 11, e = tr.find(')', s);
      if (e != std::string::npos) {
        float tx =
            parse_px(tr.substr(s, e - s), containing_block.content.width);
        dimensions.content.x += tx;
        offset_children(this, tx, 0);
      }
    }
    // translateY(...)
    size_t ty_p = tr.find("translateY(");
    if (ty_p != std::string::npos) {
      size_t s = ty_p + 11, e = tr.find(')', s);
      if (e != std::string::npos) {
        float ty =
            parse_px(tr.substr(s, e - s), containing_block.content.height);
        dimensions.content.y += ty;
        offset_children(this, 0, ty);
      }
    }
  }
}

/* Block width */

void LayoutBox::calculate_block_width(Dimensions containing_block) {

  auto &style = style_node->specified_values;

  bool has_explicit_width = style.count("width") && !style.at("width").empty()
                            && style.at("width") != "auto";

  // margin-left / margin-right — support "auto" for centering
  std::string ml_str =
      style.count("margin-left") ? style.at("margin-left") : "0";
  std::string mr_str =
      style.count("margin-right") ? style.at("margin-right") : "0";

  bool ml_auto = (ml_str == "auto");
  bool mr_auto = (mr_str == "auto");

  dimensions.margin.left =
      ml_auto ? 0 : parse_px(ml_str, containing_block.content.width);
  dimensions.margin.right =
      mr_auto ? 0 : parse_px(mr_str, containing_block.content.width);

  // margin-top / margin-bottom
  dimensions.margin.top =
      style_value(style, "margin-top", 0, containing_block.content.width);
  dimensions.margin.bottom =
      style_value(style, "margin-bottom", 0, containing_block.content.width);

  dimensions.padding.left =
      style_value(style, "padding-left", 0, containing_block.content.width);
  dimensions.padding.right =
      style_value(style, "padding-right", 0, containing_block.content.width);
  dimensions.padding.top =
      style_value(style, "padding-top", 0, containing_block.content.width);
  dimensions.padding.bottom =
      style_value(style, "padding-bottom", 0, containing_block.content.width);

  // Parse border widths into dimensions.border for correct box model
  {
    auto parse_bw = [](const std::string &s) -> float {
      if (s.empty() || s == "none" || s == "0") return 0.f;
      // border shorthand: "1px solid #999" — extract first numeric token
      try { return std::stof(s); } catch (...) { return 0.f; }
    };
    float bw = 0;
    std::string bs = style.count("border") ? style.at("border") : "";
    if (!bs.empty() && bs != "none") {
      bw = parse_bw(bs);
      dimensions.border.left = dimensions.border.right =
      dimensions.border.top = dimensions.border.bottom = bw;
    }
    // Per-side overrides
    auto read_side = [&](const char *key, float &out) {
      if (style.count(key)) {
        const std::string &v = style.at(key);
        if (!v.empty() && v != "none") out = parse_bw(v);
      }
    };
    read_side("border-top", dimensions.border.top);
    read_side("border-right", dimensions.border.right);
    read_side("border-bottom", dimensions.border.bottom);
    read_side("border-left", dimensions.border.left);
    // Explicit width longhands
    auto read_bw = [&](const char *key, float &out) {
      if (style.count(key)) {
        const std::string &v = style.at(key);
        if (v == "thin") out = 1.f;
        else if (v == "medium") out = 3.f;
        else if (v == "thick") out = 5.f;
        else if (!v.empty() && v != "none") {
          try { out = std::stof(v); } catch (...) {}
        }
      }
    };
    read_bw("border-top-width", dimensions.border.top);
    read_bw("border-right-width", dimensions.border.right);
    read_bw("border-bottom-width", dimensions.border.bottom);
    read_bw("border-left-width", dimensions.border.left);
    // border-style: none suppresses
    std::string bstyle = style.count("border-style") ? style.at("border-style") : "";
    if (bstyle == "none" || bstyle == "hidden")
      dimensions.border.left = dimensions.border.right =
      dimensions.border.top = dimensions.border.bottom = 0;
  }

  // Replaced elements (img, video) use intrinsic dimensions, not auto=fill
  bool is_replaced = false;
  float intrinsic_w = 0, intrinsic_h = 0;
  if (style_node->node && style_node->node->type == NodeType::Element) {
    const std::string &tag = style_node->node->data;
    if (tag == "img" || tag == "video") {
      is_replaced = true;
      auto &attrs = style_node->node->attributes;
      // Try HTML width/height attributes first
      if (attrs.count("width"))
        intrinsic_w = (float)atof(attrs.at("width").c_str());
      if (attrs.count("height"))
        intrinsic_h = (float)atof(attrs.at("height").c_str());
      // Try cached image dimensions for natural size
      if (tag == "img" && (intrinsic_w <= 0 || intrinsic_h <= 0)) {
        std::string src;
        if (attrs.count("src")) src = attrs.at("src");
        if (!src.empty()) {
          EnterCriticalSection(&g_image_cache_cs);
          auto it = g_image_cache.find(src);
          if (it != g_image_cache.end() && it->second.width > 0) {
            if (intrinsic_w <= 0) intrinsic_w = (float)it->second.width;
            if (intrinsic_h <= 0) intrinsic_h = (float)it->second.height;
          }
          LeaveCriticalSection(&g_image_cache_cs);
        }
      }
    }
  }

  if (has_explicit_width) {
    dimensions.content.width =
        parse_px(style.at("width"), containing_block.content.width);
    // Box-sizing: border-box
    if (style.count("box-sizing") && style.at("box-sizing") == "border-box") {
      dimensions.content.width -=
          (dimensions.padding.left + dimensions.padding.right +
           dimensions.border.left + dimensions.border.right);
      if (dimensions.content.width < 0)
        dimensions.content.width = 0;
    }
  } else if (is_replaced && intrinsic_w > 0) {
    // Replaced elements: use intrinsic width, don't stretch to container
    dimensions.content.width = intrinsic_w;
  } else {
    // Auto width: fill available space minus own padding + margin
    dimensions.content.width = containing_block.content.width
        - dimensions.padding.left - dimensions.padding.right
        - dimensions.margin.left - dimensions.margin.right
        - dimensions.border.left - dimensions.border.right;
    if (dimensions.content.width < 0)
      dimensions.content.width = 0;
  }

  // min-width / max-width clamping
  if (style.count("min-width")) {
    float mn = parse_px(style.at("min-width"), containing_block.content.width);
    if (dimensions.content.width < mn)
      dimensions.content.width = mn;
  }
  if (style.count("max-width")) {
    std::string mxs = style.at("max-width");
    if (mxs != "none" && !mxs.empty()) {
      float mx = parse_px(mxs, containing_block.content.width);
      if (mx > 0 && dimensions.content.width > mx)
        dimensions.content.width = mx;
    }
  }

  // Compute auto margins for centering (both auto = equal split)
  if (ml_auto && mr_auto) {
    float used = dimensions.content.width + dimensions.padding.left +
                 dimensions.padding.right + dimensions.border.left +
                 dimensions.border.right;
    float remaining = std::max(0.0f, containing_block.content.width - used);
    dimensions.margin.left = dimensions.margin.right = remaining / 2.0f;
  } else if (ml_auto) {
    float used = dimensions.content.width + dimensions.padding.left +
                 dimensions.padding.right + dimensions.border.left +
                 dimensions.border.right + dimensions.margin.right;
    dimensions.margin.left =
        std::max(0.0f, containing_block.content.width - used);
  } else if (mr_auto) {
    float used = dimensions.content.width + dimensions.padding.left +
                 dimensions.padding.right + dimensions.border.left +
                 dimensions.border.right + dimensions.margin.left;
    dimensions.margin.right =
        std::max(0.0f, containing_block.content.width - used);
  }
}

/* Position */

void LayoutBox::calculate_block_position(Dimensions containing_block) {

  auto &d = dimensions;

  d.content.x = containing_block.content.x + d.margin.left + d.border.left + d.padding.left;

  d.content.y = containing_block.content.y + containing_block.content.height +
                d.margin.top + d.border.top + d.padding.top;
}

// get_font_size(), get_font_weight_bold(), get_font_italic()
// moved to css_values.cpp (Split B)

// Inline layout free functions (css_line_height, collapse_whitespace,
// tokenize_words, commit_line, layout_inline_flow) moved to layout_inline.cpp
// (Split C)

// Forward declaration for css_line_height used by layout_inline_children below
float css_line_height(LayoutBox *box, int fs);

// commit_line(), layout_inline_flow() moved to layout_inline.cpp (Split C)


/* Layout children */

void LayoutBox::layout_block_children() {

  float cursor_y = dimensions.content.y;

  // Float tracking: store placed floats so non-float children can flow around
  // them
  struct FloatBox {
    float x, bottom, width;
    bool is_left;
  };
  std::vector<FloatBox> float_boxes;

  // Returns available [left_x, right_x] for content at a given y
  auto float_bounds = [&](float y) -> std::pair<float, float> {
    float lx = dimensions.content.x;
    float rx = dimensions.content.x + dimensions.content.width;
    for (const auto &fb : float_boxes) {
      if (fb.bottom <= y)
        continue; // float has cleared
      if (fb.is_left)
        lx = std::max(lx, fb.x + fb.width);
      else
        rx = std::min(rx, fb.x);
    }
    return std::make_pair(lx, rx);
  };

  // Determine if we have any block children (or floated children, which
  // generate block boxes per CSS spec and need the mixed block+inline path)
  bool has_block_child = false;
  bool any_content = false;
  for (auto &child : children) {
    if (!child)
      continue;
    // Whitespace-only text nodes between block elements don't count as content
    if (child->box_type == BoxType::InlineNode && child->style_node &&
        child->style_node->node &&
        child->style_node->node->type == NodeType::Text) {
      const std::string &txt = child->style_node->node->data;
      bool only_ws = true;
      for (char c : txt) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          only_ws = false;
          break;
        }
      }
      if (only_ws)
        continue;
    }
    any_content = true;
    if (child->box_type != BoxType::InlineNode) {
      has_block_child = true;
      break;
    }
    // Floated elements generate block boxes — treat as block child
    if (child->style_node) {
      std::string fv = child->style_node->value("float");
      if (fv == "left" || fv == "right") {
        has_block_child = true;
        break;
      }
    }
  }

  // Pure inline container — use inline flow
  if (!has_block_child && any_content) {
    layout_inline_flow(this);
    return;
  }

  // Mixed or pure block: process children in order.
  // Consecutive inline children are grouped into a temporary anonymous block
  // and laid out together via layout_inline_flow so they flow horizontally.
  std::vector<std::shared_ptr<LayoutBox>> inline_run;

  auto flush_inline_run = [&]() {
    if (inline_run.empty())
      return;

    // Lay out each inline child then position them in a horizontal run
    // Account for active floats when positioning
    std::pair<float,float> fb = float_bounds(cursor_y);
    float cx = fb.first;
    float cy = cursor_y;
    float max_x = fb.second;
    dbg("[FLUSH-INLINE] %zu items, float_bounds=[%.0f,%.0f] at y=%.0f (container x=%.0f w=%.0f) floats=%zu\n",
            inline_run.size(), fb.first, fb.second, cursor_y, dimensions.content.x, dimensions.content.width, float_boxes.size());
    float row_h = 16.0f;

    // Resolve text-align from parent style (inherited)
    std::string ta;
    if (style_node) ta = style_node->value("text-align");

    // Track items per row for text-align centering/right
    struct RowItem { size_t idx; float old_x; float old_y; };
    std::vector<RowItem> current_row;
    float row_start_x = cx;

    auto apply_text_align = [&]() {
      if (current_row.empty() || ta.empty() || ta == "left") return;
      // Find rightmost edge in this row
      float used = 0;
      for (auto &ri2 : current_row) {
        auto &ic2 = inline_run[ri2.idx];
        if (!ic2) continue;
        float right = (ic2->dimensions.content.x - row_start_x) +
                      ic2->dimensions.margin_box().width;
        used = std::max(used, right);
      }
      float slack = dimensions.content.width - used;
      if (slack <= 0) return;
      float shift = 0;
      if (ta == "center") shift = slack / 2.f;
      else if (ta == "right") shift = slack;
      if (shift > 0) {
        for (auto &ri2 : current_row) {
          auto &ic2 = inline_run[ri2.idx];
          if (!ic2) continue;
          ic2->dimensions.content.x += shift;
          offset_children(ic2.get(), shift, 0.f);
        }
      }
    };

    for (size_t ri = 0; ri < inline_run.size(); ++ri) {
      std::shared_ptr<LayoutBox> &ic = inline_run[ri];
      if (!ic)
        continue;

      // <br> forces a line break
      bool is_br = ic->style_node && ic->style_node->node &&
                   ic->style_node->node->type == NodeType::Element &&
                   ic->style_node->node->data == "br";
      if (is_br) {
        apply_text_align();
        current_row.clear();
        cy += row_h;
        row_h = 16.0f;
        // Recalculate float bounds for the new line
        std::pair<float,float> nfb = float_bounds(cy);
        cx = nfb.first;
        max_x = nfb.second;
        row_start_x = cx;
        continue;
      }

      Dimensions ic_cb = dimensions;
      ic_cb.content.height = 0;
      ic->layout(ic_cb);
      if (ic->box_type == BoxType::InlineNode) {
        float child_max_x = 0.f;
        for (auto &ch : ic->children) {
          if (!ch)
            continue;
          child_max_x =
              std::max(child_max_x, ch->dimensions.content.x +
                                        ch->dimensions.margin_box().width);
        }
        if (child_max_x > ic->dimensions.content.width) {
          ic->dimensions.content.width = child_max_x;
        }
      }

      float w = ic->dimensions.content.width + ic->dimensions.padding.left +
                ic->dimensions.padding.right + ic->dimensions.margin.left +
                ic->dimensions.margin.right;
      float h = ic->dimensions.margin_box().height;
      if (h < 1)
        h = 16.0f;

      if (cx + w > max_x && cx > fb.first) {
        apply_text_align();
        current_row.clear();
        cy += row_h;
        row_h = h;
        // Recalculate float bounds for the new line
        std::pair<float,float> nfb = float_bounds(cy);
        cx = nfb.first;
        max_x = nfb.second;
        row_start_x = cx;
      }
      float old_x = ic->dimensions.content.x;
      float old_y = ic->dimensions.content.y;
      ic->dimensions.content.x = cx;
      ic->dimensions.content.y = cy;
      float ddx = ic->dimensions.content.x - old_x;
      float ddy = ic->dimensions.content.y - old_y;
      if (ddx != 0.f || ddy != 0.f)
        offset_children(ic.get(), ddx, ddy);

      current_row.push_back({ri, old_x, old_y});
      row_h = std::max(row_h, h);
      cx += w;
    }
    apply_text_align();
    cursor_y = cy + row_h;
    inline_run.clear();
  };

  for (auto &child : children) {
    if (!child)
      continue;

    // Skip whitespace-only text nodes ONLY when between block elements.
    // Between inline elements, whitespace represents a real space (e.g.
    // <a>English</a> <a>Hindi</a> — the space between must be preserved).
    if (child->box_type == BoxType::InlineNode && child->style_node &&
        child->style_node->node &&
        child->style_node->node->type == NodeType::Text) {
      const std::string &txt = child->style_node->node->data;
      bool only_ws = true;
      for (char c : txt) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
          only_ws = false;
          break;
        }
      }
      if (only_ws) {
        // Check if this whitespace is between inline siblings — if so, keep it
        // as a single space. Only skip if all surrounding siblings are blocks.
        bool has_inline_neighbor = !inline_run.empty(); // already in an inline run
        if (!has_inline_neighbor) {
          // Look ahead to see if next non-whitespace sibling is inline
          size_t ci = 0;
          for (size_t i = 0; i < children.size(); ++i) {
            if (children[i].get() == child.get()) { ci = i; break; }
          }
          for (size_t i = ci + 1; i < children.size(); ++i) {
            if (!children[i]) continue;
            if (children[i]->box_type == BoxType::InlineNode) {
              has_inline_neighbor = true;
            }
            break; // only check immediate next
          }
        }
        if (!has_inline_neighbor)
          continue; // skip — whitespace between block elements
        // Otherwise collapse to a single space and keep in inline run
        child->style_node->node->data = " ";
      }
    }

    if (child->box_type == BoxType::InlineNode) {
      inline_run.push_back(child);
    } else {
      std::string child_pos =
          child->style_node ? child->style_node->value("position") : "static";
      std::string child_float =
          child->style_node ? child->style_node->value("float") : "";

      // Handle clear: left/right/both
      std::string child_clear =
          child->style_node ? child->style_node->value("clear") : "";
      if (!child_clear.empty() && child_clear != "none") {
        float clear_y = cursor_y;
        for (const auto &fb : float_boxes) {
          if ((child_clear == "left" && fb.is_left) ||
              (child_clear == "right" && !fb.is_left) ||
              child_clear == "both") {
            clear_y = std::max(clear_y, fb.bottom);
          }
        }
        cursor_y = clear_y;
      }

      if (child_float == "left" || child_float == "right") {
        // Float: out-of-flow, positioned at current float edge
        flush_inline_run();
        std::pair<float, float> fb_bounds = float_bounds(cursor_y);
        float flx = fb_bounds.first, frx = fb_bounds.second;
        float avail_for_float = frx - flx;
        Dimensions child_cb = dimensions;
        child_cb.content.width = avail_for_float; // constrain float to available space
        child_cb.content.height = 0;
        child->layout(child_cb);
        float fw = child->dimensions.margin_box().width;
        float fh = child->dimensions.margin_box().height;
        // If float doesn't fit beside existing floats, drop below them
        if (fw > avail_for_float + 0.5f && !float_boxes.empty()) {
          float drop_y = cursor_y;
          for (const auto &efb : float_boxes)
            drop_y = std::max(drop_y, efb.bottom);
          cursor_y = drop_y;
          fb_bounds = float_bounds(cursor_y);
          flx = fb_bounds.first;
          frx = fb_bounds.second;
        }
        if (child_float == "left") {
          float new_x = flx + child->dimensions.margin.left +
                        child->dimensions.border.left + child->dimensions.padding.left;
          float new_y = cursor_y + child->dimensions.margin.top +
                        child->dimensions.border.top + child->dimensions.padding.top;
          float dx = new_x - child->dimensions.content.x;
          float dy = new_y - child->dimensions.content.y;
          child->dimensions.content.x = new_x;
          child->dimensions.content.y = new_y;
          if (dx != 0.f || dy != 0.f)
            offset_children(child.get(), dx, dy);
          FloatBox newf;
          newf.x = flx;
          newf.bottom = cursor_y + fh;
          newf.width = fw;
          newf.is_left = true;
          float_boxes.push_back(newf);
        } else {
          float new_x = frx - fw + child->dimensions.margin.left +
                        child->dimensions.border.left + child->dimensions.padding.left;
          float new_y = cursor_y + child->dimensions.margin.top +
                        child->dimensions.border.top + child->dimensions.padding.top;
          float dx = new_x - child->dimensions.content.x;
          float dy = new_y - child->dimensions.content.y;
          child->dimensions.content.x = new_x;
          child->dimensions.content.y = new_y;
          if (dx != 0.f || dy != 0.f)
            offset_children(child.get(), dx, dy);
          FloatBox newf;
          newf.x = frx - fw;
          newf.bottom = cursor_y + fh;
          newf.width = fw;
          newf.is_left = false;
          float_boxes.push_back(newf);
        }
        // Floats don't advance cursor_y
        dbg("[FLOAT] %s float placed: x=%.0f y=%.0f w=%.0f h=%.0f | container=%.0f avail=%.0f\n",
                child_float.c_str(), child->dimensions.content.x, child->dimensions.content.y, fw, fh,
                dimensions.content.width, avail_for_float);
      } else {
        // Flush pending inline run first
        flush_inline_run();

        // Compute available content region accounting for floats
        std::pair<float, float> b_bounds = float_bounds(cursor_y);
        float blx = b_bounds.first, brx = b_bounds.second;
        Dimensions child_cb = dimensions;
        child_cb.content.x = blx;
        child_cb.content.width = brx - blx;
        child_cb.content.height = cursor_y - dimensions.content.y;
        dbg("[BLOCK-BESIDE-FLOAT] tag=%s x=%.0f w=%.0f (container w=%.0f, float_bounds=[%.0f,%.0f])\n",
                child->style_node && child->style_node->node ? child->style_node->node->data.c_str() : "?",
                blx, brx - blx, dimensions.content.width, blx, brx);
        child->layout(child_cb);

        if (child_pos != "absolute" && child_pos != "fixed") {
          cursor_y = child->dimensions.margin_box().y +
                     child->dimensions.margin_box().height;
        }
      }
    }
  }
  flush_inline_run();

  // After normal flow, make sure height clears all floats too
  for (const auto &fb : float_boxes)
    cursor_y = std::max(cursor_y, fb.bottom);

  dimensions.content.height = cursor_y - dimensions.content.y;
}

void LayoutBox::layout_inline_children() {
  auto &fm = FontMetrics::get_instance();
  int pfs = get_font_size(style_node);
  bool bold = get_font_weight_bold(style_node);
  float lh = css_line_height(this, pfs);

  // All children are positioned RELATIVE to this box's content origin (0,0).
  // The parent's flush_inline_run will later shift everything to absolute
  // page coordinates via offset_children().

  float cx = 0.f; // cursor x relative to (0,0)
  float max_cx = 0.f;
  float row_h = lh;

  for (auto &child : children) {
    if (!child)
      continue;

    bool is_text = child->style_node && child->style_node->node &&
                   child->style_node->node->type == NodeType::Text;

    if (is_text) {
      int fs = get_font_size(child->style_node);
      bool cbold = get_font_weight_bold(child->style_node);
      float clh = css_line_height(child.get(), fs);

      std::string collapsed =
          collapse_whitespace(child->style_node->node->data);
      if (collapsed.empty()) {
        child->dimensions.content = Rect(cx, 0.f, 0.f, (float)fs);
        continue;
      }

      bool citalic = get_font_italic(child->style_node);
      auto metrics = fm.measure_text(collapsed, fs, cbold, 0.f, citalic);
      child->dimensions.content.x = cx;
      child->dimensions.content.y = 0.f;
      child->dimensions.content.width = metrics.width;
      child->dimensions.content.height = (float)fs;

      row_h = std::max(row_h, clh);
      cx += metrics.width;
    } else {
      // Nested inline element (e.g. <a>, <span>, <em> inside another inline)
      float child_lh =
          css_line_height(child.get(), get_font_size(child->style_node));

      // Replaced elements (img, video) need full layout() to get intrinsic dimensions
      bool is_replaced_elem = child->style_node && child->style_node->node &&
          child->style_node->node->type == NodeType::Element &&
          (child->style_node->node->data == "img" ||
           child->style_node->node->data == "video");
      if (is_replaced_elem) {
        // Use parent's containing block so percentage widths resolve properly
        Dimensions img_cb = dimensions;
        if (img_cb.content.width < 1) {
          // Inline parent has shrink-to-fit width; use a large reference
          // so the img can use its HTML attributes for sizing
          img_cb.content.width = 10000;
        }
        child->layout(img_cb);
      } else {
        // Recursively layout the nested inline's children relative to (0,0)
        child->layout_inline_children();
      }

      // Position the nested inline relative to our cursor
      float child_ml = child->dimensions.margin.left;
      float child_pl = child->dimensions.padding.left;
      child->dimensions.content.x = cx + child_ml + child_pl;
      child->dimensions.content.y = 0.f;

      // Now shift all grandchildren by the delta from (0,0) to the actual position
      // since layout_inline_children placed them relative to (0,0)
      float shift_x = child->dimensions.content.x;
      if (shift_x != 0.f)
        offset_children(child.get(), shift_x, 0.f);

      float w = child->dimensions.margin_box().width;
      float h = child->dimensions.margin_box().height;

      if (h < 1.f)
        h = child_lh;

      row_h = std::max(row_h, h);
      cx += w;
    }
    max_cx = std::max(max_cx, cx);
  }

  // Shrink-wrap: content.width = max child extent, height = line height
  if (max_cx > 0.f)
    dimensions.content.width = std::max(dimensions.content.width, max_cx);
  float new_h = row_h > 0.f ? row_h : dimensions.content.height;
  dimensions.content.height = std::max(dimensions.content.height, new_h);
}

// After flush_inline_run sets a box's absolute position,
// shift all its children by (dx, dy) so they are absolutely positioned too.
void offset_children(LayoutBox *box, float dx, float dy) {
  if (!box)
    return;
  for (auto &child : box->children) {
    if (!child)
      continue;
    child->dimensions.content.x += dx;
    child->dimensions.content.y += dy;
    for (auto &ef : child->extra_fragments) {
      ef.rect.x += dx;
      ef.rect.y += dy;
    }
    offset_children(child.get(), dx, dy);
  }
}

/* Height */

void LayoutBox::calculate_block_height(float containing_height) {
  auto &style = style_node->specified_values;
  extern int g_viewport_height;
  float vp_h = g_viewport_height > 0 ? (float)g_viewport_height : 768.0f;

  // For percentage heights: use containing block for normal/relative flow.
  // Only fall back to viewport for fixed/absolute elements (their containing
  // block IS the viewport).
  std::string pos = style.count("position") ? style.at("position") : "static";
  float pct_base =
      (pos == "fixed" || pos == "absolute") ? vp_h : containing_height;

  // If an explicit height was set in CSS, use it
  if (style.count("height")) {
    const std::string &hv = style.at("height");
    float h = 0;
    if (!hv.empty() && hv.back() == '%') {
      float pct = 0;
      try {
        pct = std::stof(hv.substr(0, hv.length() - 1));
      } catch (...) {
      }
      h = pct_base * pct / 100.0f;
    } else {
      h = parse_px(hv, 0);
    }
    if (h > 0) {
      dimensions.content.height = h;
      return;
    }
  }

  // Replaced elements (img, video): use intrinsic height, preserving aspect ratio
  if (style_node->node && style_node->node->type == NodeType::Element) {
    const std::string &tag = style_node->node->data;
    if (tag == "img" || tag == "video") {
      auto &attrs = style_node->node->attributes;
      float attr_w = 0, attr_h = 0;
      if (attrs.count("width")) attr_w = (float)atof(attrs.at("width").c_str());
      if (attrs.count("height")) attr_h = (float)atof(attrs.at("height").c_str());
      // Try cached image for natural dimensions
      float nat_w = 0, nat_h = 0;
      if (tag == "img") {
        std::string src;
        if (attrs.count("src")) src = attrs.at("src");
        if (!src.empty()) {
          EnterCriticalSection(&g_image_cache_cs);
          auto it = g_image_cache.find(src);
          if (it != g_image_cache.end() && it->second.width > 0) {
            nat_w = (float)it->second.width;
            nat_h = (float)it->second.height;
          }
          LeaveCriticalSection(&g_image_cache_cs);
        }
      }
      float iw = attr_w > 0 ? attr_w : nat_w;
      float ih = attr_h > 0 ? attr_h : nat_h;
      if (ih > 0 && iw > 0) {
        // Scale height proportionally if width was constrained
        float actual_w = dimensions.content.width;
        if (actual_w > 0 && actual_w != iw) {
          dimensions.content.height = ih * (actual_w / iw);
        } else {
          dimensions.content.height = ih;
        }
        return;
      } else if (ih > 0) {
        dimensions.content.height = ih;
        return;
      }
    }
  }

  // Apply min-height
  if (style.count("min-height")) {
    const std::string &mv = style.at("min-height");
    float minh = 0;
    if (!mv.empty() && mv.back() == '%') {
      float pct = 0;
      try {
        pct = std::stof(mv.substr(0, mv.length() - 1));
      } catch (...) {
      }
      minh = pct_base * pct / 100.0f;
    } else {
      minh = parse_px(mv, 0);
    }
    if (dimensions.content.height < minh)
      dimensions.content.height = minh;
  }

  // Apply max-height
  if (style.count("max-height")) {
    const std::string &mv = style.at("max-height");
    if (mv != "none" && !mv.empty()) {
      float maxh = 0;
      if (!mv.empty() && mv.back() == '%') {
        float pct = 0;
        try {
          pct = std::stof(mv.substr(0, mv.length() - 1));
        } catch (...) {
        }
        maxh = pct_base * pct / 100.0f;
      } else {
        maxh = parse_px(mv, 0);
      }
      if (maxh > 0 && dimensions.content.height > maxh)
        dimensions.content.height = maxh;
    }
  }

  // Apply aspect-ratio if height was not explicitly set
  if (style.count("aspect-ratio") && !style.count("height")) {
    const std::string &ar_v = style.at("aspect-ratio");
    if (ar_v != "auto" && !ar_v.empty()) {
      float ratio = 0.f;
      // Formats: "16/9", "16 / 9", "1.778", "1"
      size_t slash = ar_v.find('/');
      if (slash != std::string::npos) {
        try {
          float w_part = std::stof(ar_v.substr(0, slash));
          float h_part = std::stof(ar_v.substr(slash + 1));
          if (h_part > 0)
            ratio = w_part / h_part;
        } catch (...) {
        }
      } else {
        try {
          ratio = std::stof(ar_v);
        } catch (...) {
        }
      }
      if (ratio > 0 && dimensions.content.width > 0) {
        float target_h = dimensions.content.width / ratio;
        if (target_h > dimensions.content.height) {
          dimensions.content.height = target_h;
        }
      }
    }
  }

  // Don't override height that was already computed by layout_block_children
  // Only apply a minimum if we have actual content but zero height
  if (dimensions.content.height == 0 && !children.empty()) {
    dimensions.content.height = 20;
  }
}

// Check if a DOM subtree has any visible content (non-whitespace text, images,
// form controls). Used to collapse empty autocomplete list items, empty list
// rows, etc. that have nested divs but no actual content to display.
static bool has_visible_content(const std::shared_ptr<Node> &node) {
  if (!node) return false;
  if (node->type == NodeType::Text) {
    for (char c : node->data) {
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return true;
    }
    return false;
  }
  if (node->type == NodeType::Element) {
    const std::string &tag = node->data;
    // These elements are inherently visible even without text children
    if (tag == "img" || tag == "video" ||
        tag == "input" || tag == "button" || tag == "textarea" || tag == "select")
      return true;
    // SVG/canvas are skipped in layout — don't count as visible
    if (tag == "svg" || tag == "canvas" || tag == "math") return false;
    // Skip hidden children
    if (node->attributes.count("hidden")) return false;
    for (auto &child : node->children) {
      if (has_visible_content(child)) return true;
    }
  }
  return false;
}

/* Layout tree builder */

std::shared_ptr<LayoutBox>
build_layout_tree(std::shared_ptr<StyledNode> style_node) {
  if (!style_node)
    return nullptr;

  // Skip root-level text nodes (whitespace before <html>)
  if (style_node->node && style_node->node->type == NodeType::Text &&
      style_node->children.size() == 1) {
    return build_layout_tree(style_node->children[0]);
  }

  // Respect display:none — skip element and all its children.
  // This hides Google's dialogs, upload toasts, token inputs, etc. that are
  // hidden by CSS and only shown via JS (which we can't fully run).
  if (style_node->node && style_node->node->type == NodeType::Element) {
    std::string disp = style_node->value("display");
    if (disp == "none") return nullptr;
    // visibility:hidden — element is invisible but still takes up space.
    // We render it transparently (no special handling needed here).

    // Skip JS-only absolutely/fixed positioned elements (overlays controlled by JS).
    // These have jscontroller but no CSS/HTML display:none — they'd appear in wrong
    // positions if rendered in normal flow (e.g. "Report inappropriate predictions").
    if (style_node->node->attributes.count("jscontroller")) {
      std::string pos = style_node->value("position");
      if (pos == "absolute" || pos == "fixed") return nullptr;
    }
  }

  BoxType type = BoxType::BlockNode;

  // Text nodes are always inline
  if (style_node->node && style_node->node->type == NodeType::Text) {
    type = BoxType::InlineNode;
  } else if (!style_node->node || style_node->node->type != NodeType::Element ||
             style_node->node->data.empty()) {
    // Document wrapper or node with no element tag: always block
    type = BoxType::BlockNode;
  } else {
    std::string tag = style_node->node->data;
    std::string display = style_node->value("display");

    // Skip non-visual / server-only elements
    if (tag == "script" || tag == "style" || tag == "meta" || tag == "link" ||
        tag == "head" || tag == "title" || tag == "template" ||
        tag == "noscript") {
      return nullptr;
    }
    // SVG and canvas cannot be rendered by our engine — skip entirely
    // (keeps the layout clean and avoids phantom boxes for Google's SVG logo)
    if (tag == "svg" || tag == "canvas" || tag == "math") {
      return nullptr;
    }
    // input[type=hidden] — never visible, never takes space
    if (tag == "input") {
      std::string itype;
      if (style_node->node->attributes.count("type"))
        itype = style_node->node->attributes.at("type");
      // lowercase comparison
      for (char &c : itype) c = (char)tolower((unsigned char)c);
      if (itype == "hidden") return nullptr;
    }
    // HTML hidden attribute — treat like display:none
    if (style_node->node->attributes.count("hidden")) {
      return nullptr;
    }
    // <dialog> is hidden by default unless it has the 'open' attribute
    if (tag == "dialog") {
      bool has_open = style_node->node->attributes.count("open") > 0;
      if (!has_open)
        return nullptr;
    }
    // Collapse empty list items — Google's autocomplete creates <li> elements
    // with nested divs but no visible text/images when there are no suggestions.
    // These waste vertical space (from padding) without displaying anything.
    if (tag == "li" && !has_visible_content(style_node->node)) {
      return nullptr;
    }
    // <br>: forced line break
    if (tag == "br") {
      auto br_box =
          std::make_shared<LayoutBox>(BoxType::InlineNode, style_node);
      br_box->dimensions.content.width = 0;
      br_box->dimensions.content.height = 20;
      return br_box;
    }

    if (display == "flex" || display == "inline-flex") {
      type = BoxType::FlexContainer;
    } else if (display == "grid" || display == "inline-grid") {
      type = BoxType::GridContainer;
    } else if (display == "none") {
      // Respect display:none — skip the element and all its children.
      // Previously we rendered these hoping JS would un-hide them, but that
      // caused modals, popups and tooltips to appear on every page load.
      return nullptr;
    } else if (display == "inline" || display == "inline-block") {
      type = BoxType::InlineNode;
    } else if (display == "table" || display == "inline-table" ||
               tag == "table") {
      type = BoxType::TableContainer;
    } else if (display == "table-row" || tag == "tr" ||
               display == "table-cell" || tag == "td" || tag == "th" ||
               display == "table-header-group" || display == "table-row-group" ||
               display == "table-footer-group" ||
               tag == "thead" || tag == "tbody" || tag == "tfoot" ||
               display == "table-caption") {
      type = BoxType::BlockNode;
    } else if (display == "block" || display == "list-item" ||
               display == "table-caption") {
      type = BoxType::BlockNode;
    } else if (is_inline_tag(tag)) {
      type = BoxType::InlineNode;
    }
    // else: type stays BlockNode (default for unknown/empty display)
  }

  auto root = std::make_shared<LayoutBox>(type, style_node);

  for (auto &child : style_node->children) {
    auto c = build_layout_tree(child);
    if (c)
      root->children.push_back(c);
  }

  return root;
}

/* Debug functions removed — use out.txt display list instead */
