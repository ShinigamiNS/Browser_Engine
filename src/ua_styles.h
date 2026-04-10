#pragma once
// ua_styles.h — UA (user-agent) default stylesheet application
// Moved from style.cpp (Split E)
#include "style.h"
#include <string>

// Apply UA default styles to a StyledNode for the given HTML tag.
// Only sets properties that are not already specified (lowest priority).
// node->node must be non-null and of type NodeType::Element.
void apply_ua_defaults(StyledNode *node, const std::string &tag);
