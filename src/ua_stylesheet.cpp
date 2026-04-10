// ua_stylesheet.cpp — Parsed UA (user-agent) default stylesheet.
//
// This file mirrors what Gecko's layout/style/res/html.css does: a real
// CSS stylesheet that is matched through the normal selector engine, so
// descendant selectors (e.g. `ul ul { list-style-type: circle }`) and
// attribute selectors (e.g. `[hidden] { display: none }`) work without
// any special-case C++ logic.
//
// The cascade in style.cpp applies this UA sheet first, so author rules
// naturally win unless they have lower specificity. Force-overrides
// (display:none for <script>, <style>, etc.) still live in ua_styles.cpp
// because they need to win over author rules unconditionally.

#include "ua_stylesheet.h"
#include "css_parser.h"

namespace {

// Use longhands wherever possible — until the parse-time shorthand
// expansion refactor lands, the post-cascade shorthand expansion in
// style.cpp can interact awkwardly with longhands set by other rules.
const char *kUACss = R"CSS(
/* ── Document & body ─────────────────────────────────────────────── */
html { display: block; }
body {
  display: block;
  margin-top: 8px; margin-right: 8px; margin-bottom: 8px; margin-left: 8px;
}

/* ── Block-level structural elements ─────────────────────────────── */
div, section, article, nav, header, footer, main, aside,
form, fieldset, figure, figcaption, details, summary, dialog,
address, hgroup, search { display: block; }

p {
  display: block;
  margin-top: 16px; margin-bottom: 16px;
}

blockquote {
  display: block;
  margin-top: 16px; margin-bottom: 16px;
  margin-left: 40px; margin-right: 40px;
}

pre {
  display: block;
  white-space: pre;
  font-family: monospace;
  margin-top: 16px; margin-bottom: 16px;
}

hr {
  display: block;
  border-top: 1px solid #ccc;
  margin-top: 8px; margin-bottom: 8px;
}

/* ── Headings ────────────────────────────────────────────────────── */
h1 { display: block; font-weight: bold; font-size: 32px; }
h2 { display: block; font-weight: bold; font-size: 24px; }
h3 { display: block; font-weight: bold; font-size: 20px; }
h4 { display: block; font-weight: bold; font-size: 16px; }
h5 { display: block; font-weight: bold; font-size: 13px; }
h6 { display: block; font-weight: bold; font-size: 11px; }

/* ── Inline emphasis & decoration ────────────────────────────────── */
em, i, cite, var, dfn { font-style: italic; }
b, strong { font-weight: bold; }
u, ins { text-decoration: underline; }
s, del, strike { text-decoration: line-through; }
small { font-size: 13px; }
sub { font-size: smaller; vertical-align: sub; }
sup { font-size: smaller; vertical-align: super; }
mark { background-color: yellow; color: black; }
a { text-decoration: underline; }
center { display: block; text-align: center; }

/* ── Code ────────────────────────────────────────────────────────── */
code, kbd, samp, tt { font-family: monospace; }

/* ── Lists ───────────────────────────────────────────────────────── */
ul, ol {
  display: block;
  padding-left: 40px;
  margin-top: 8px; margin-bottom: 8px;
}
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
li {
  display: list-item;
  margin-top: 2px; margin-bottom: 2px;
}

/* Nested list markers — disc → circle → square → square …
   This is the rule set Gecko's html.css uses. */
ul ul, ol ul { list-style-type: circle; }
ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square; }

/* Definition lists */
dl {
  display: block;
  margin-top: 16px; margin-bottom: 16px;
}
dt { display: block; }
dd { display: block; margin-left: 40px; }

/* ── Tables ──────────────────────────────────────────────────────── */
table {
  display: table;
  border-collapse: separate;
  border-spacing: 2px;
}
thead, tbody, tfoot { display: table-row-group; }
tr { display: table-row; }
td, th {
  display: table-cell;
  padding-top: 1px; padding-right: 1px; padding-bottom: 1px; padding-left: 1px;
  vertical-align: middle;
}
th { font-weight: bold; text-align: center; }
caption { display: table-caption; text-align: center; }
colgroup, col { display: none; }

/* ── Form controls ───────────────────────────────────────────────── */
input, button, textarea, select { display: inline-block; }
fieldset {
  display: block;
  border-top: 2px groove #ddd; border-right: 2px groove #ddd;
  border-bottom: 2px groove #ddd; border-left: 2px groove #ddd;
  padding-top: 6px; padding-right: 10px; padding-bottom: 6px; padding-left: 10px;
}
legend { display: block; padding-left: 2px; padding-right: 2px; }

/* ── Embedded content ────────────────────────────────────────────── */
iframe {
  border-top: 2px inset #ddd; border-right: 2px inset #ddd;
  border-bottom: 2px inset #ddd; border-left: 2px inset #ddd;
}

/* ── Things that must always be invisible by default ─────────────── */
script, style, head, meta, link, noscript, template,
area, datalist, optgroup, option, source, track, map { display: none; }
[hidden] { display: none; }
input[type=hidden] { display: none; }
)CSS";

Stylesheet *g_ua_sheet = nullptr;
RuleIndex *g_ua_index = nullptr;

void ensure_built() {
  if (g_ua_sheet) return;
  CSSParser p(kUACss);
  g_ua_sheet = new Stylesheet(p.parse_stylesheet());
  g_ua_index = new RuleIndex(build_rule_index(*g_ua_sheet));
}

} // namespace

const Stylesheet &get_ua_stylesheet() {
  ensure_built();
  return *g_ua_sheet;
}

const RuleIndex &get_ua_rule_index() {
  ensure_built();
  return *g_ua_index;
}
