#pragma once
// ua_stylesheet.h — Parsed UA (user-agent) default stylesheet
// Provides a real CSS stylesheet of UA defaults so the cascade can apply
// them through the normal selector matching path. Replaces most of the
// per-tag set_default rules that used to live in ua_styles.cpp.
#include "css.h"
#include "selector.h"

// Returns the parsed UA stylesheet (lazy-built on first call).
const Stylesheet &get_ua_stylesheet();

// Returns a rule index for the UA stylesheet (lazy-built on first call).
const RuleIndex &get_ua_rule_index();
