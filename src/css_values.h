#pragma once
// css_values.h — CSS value parsing helpers
// Moved from layout.cpp (Split B)
#include "style.h"
#include <string>

// Parse a CSS length value (px, em, rem, %, vw, vh, vmin, vmax, ch,
// calc(), clamp(), min(), max()) to a float pixel value.
// relative_to is the containing-block width used for % resolution.
float parse_px(const std::string &v, float relative_to = 0);

// Look up a property in a PropertyMap and parse it as a pixel value.
float style_value(const PropertyMap &map, const std::string &key,
                  float def, float relative_to = 0);

// Resolve font-size from a StyledNode to an integer pixel value.
int get_font_size(const std::shared_ptr<StyledNode> &node);

// Return true if the node's effective font-weight is bold.
bool get_font_weight_bold(const std::shared_ptr<StyledNode> &node);

// Return true if the node's effective font-style is italic/oblique.
bool get_font_italic(const std::shared_ptr<StyledNode> &node);
