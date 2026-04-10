#pragma once
#include "style.h"
#include <memory>
#include <string>
#include <vector>


struct Rect {
  float x;
  float y;
  float width;
  float height;
  Rect() : x(0), y(0), width(0), height(0) {}
  Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), width(w_), height(h_) {}
};

struct EdgeSizes {
  float left;
  float right;
  float top;
  float bottom;
  EdgeSizes() : left(0), right(0), top(0), bottom(0) {}
};

struct Dimensions {
  Rect content;
  EdgeSizes padding;
  EdgeSizes border;
  EdgeSizes margin;

  Rect padding_box() const {
    return {content.x - padding.left, content.y - padding.top,
            content.width + padding.left + padding.right,
            content.height + padding.top + padding.bottom};
  }

  Rect border_box() const {
    Rect pad_box = padding_box();
    return {pad_box.x - border.left, pad_box.y - border.top,
            pad_box.width + border.left + border.right,
            pad_box.height + border.top + border.bottom};
  }

  Rect margin_box() const {
    Rect bord_box = border_box();
    return {bord_box.x - margin.left, bord_box.y - margin.top,
            bord_box.width + margin.left + margin.right,
            bord_box.height + margin.top + margin.bottom};
  }
};

enum class BoxType { BlockNode, InlineNode, FlexContainer, GridContainer, TableContainer, AnonymousBlock };

// Extra text line fragment (for wrapped text nodes)
struct TextFragment {
  Rect rect;          // position and size of this fragment
  std::string text;   // text content for this fragment
};

class LayoutBox {
public:
  Dimensions dimensions;
  BoxType box_type;
  std::shared_ptr<StyledNode> style_node;
  std::vector<std::shared_ptr<LayoutBox>> children;
  std::vector<TextFragment> extra_fragments; // additional line fragments for wrapped text

  // Optimization: only re-layout if dirty
  bool is_dirty = true;

  LayoutBox(BoxType type, std::shared_ptr<StyledNode> node)
      : box_type(type), style_node(node) {}

  void layout(Dimensions containing_block);
  
  void mark_dirty() {
    is_dirty = true;
    // Don't need to bubble up for simple Robinson engine, but useful for later
  }

private:
  void calculate_block_width(Dimensions containing_block);
  void calculate_block_position(Dimensions containing_block);
  void layout_block_children();
  void layout_inline_children();
  void calculate_block_height(float containing_height = 0);

  // Formatting Contexts (Phase 2)
  friend class BlockFormattingContext;
  friend class InlineFormattingContext;
  friend class FlexFormattingContext;
};

std::shared_ptr<LayoutBox>
build_layout_tree(std::shared_ptr<StyledNode> style_node);

// Inline layout entry point — called from layout_block_children()
// Defined in layout_inline.cpp
void layout_inline_flow(LayoutBox *parent);

// Shift all children of box by (dx, dy) — used by inline and flex layout
// Defined in layout.cpp
void offset_children(LayoutBox *box, float dx, float dy);

// Collapse runs of whitespace to a single space (CSS white-space:normal).
// Defined in layout_inline.cpp; also used by layout_inline_children() in layout.cpp
std::string collapse_whitespace(const std::string &s);
