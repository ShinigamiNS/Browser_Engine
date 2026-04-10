// ua_styles.cpp — UA force-override rules
//
// Most UA defaults now live in ua_stylesheet.cpp as a real parsed CSS
// stylesheet that gets fed through the cascade. The cases below MUST
// override author CSS unconditionally (e.g. a page can't make a <script>
// element render), so they stay as direct C++ assignments applied AFTER
// the author cascade in style.cpp.
#include "ua_styles.h"
#include "selector.h"  // for lower()
#include <cstring>

void apply_ua_defaults(StyledNode *node, const std::string &tag) {
  if (!node) return;
  auto &sv = node->specified_values;

  // Elements that must never be rendered, regardless of author CSS.
  if (tag == "script" || tag == "style" || tag == "head" || tag == "meta" ||
      tag == "link"   || tag == "noscript" || tag == "template" ||
      tag == "svg"    || tag == "canvas"   || tag == "object" || tag == "param") {
    sv["display"] = "none";
  }
  // Custom notification/toast elements — always hidden until JS shows them;
  // since we can't run full JS, keep them hidden.
  if (tag == "g-snackbar" || tag == "g-bubble" ||
      tag == "g-dialog"   || tag == "g-fab") {
    sv["display"] = "none";
  }
  // g-popup: allow the trigger (e.g. Settings button) to show;
  // the dropdown menu child has its own display:none in inline styles.
  // g-menu: render as block (its parent g-popup dropdown hides it via inline style).
  if (tag == "g-popup") {
    sv["display"] = "inline-block";
  }
  if (tag == "g-menu") {
    sv["display"] = "block";
  }
  // Google-specific class overrides: elements normally hidden by JS-loaded CSS
  // that we can't fetch (Google loads external CSS via JS).
  if (node->node) {
    auto cls_it = node->node->attributes.find("class");
    if (cls_it != node->node->attributes.end()) {
      const std::string &cls = cls_it->second;
      auto has = [&](const char *c) {
        size_t p = cls.find(c);
        if (p == std::string::npos) return false;
        size_t e = p + strlen(c);
        bool before_ok = (p == 0 || cls[p-1] == ' ');
        bool after_ok  = (e == cls.size() || cls[e] == ' ');
        return before_ok && after_ok;
      };
      // Clear/× button — shown only when input has content (JS-driven)
      if (has("vOY7J")) sv["display"] = "none";
      // "Report inappropriate predictions" tooltip (absolutely positioned overlay)
      if (has("WzNHm")) sv["display"] = "none";
      // Google Doodle share/social buttons (contain SVGs we can't render)
      if (has("IzOpfd")) sv["display"] = "none";  // share button container
      // Autocomplete/predictions overlay (JS-driven, empty without JS)
      if (has("UUbT9")) sv["display"] = "none";
      if (has("aajZCb")) sv["display"] = "none";
      // Google Lens camera/visual search button (SVG icon only)
      if (has("nDcEnd")) sv["display"] = "none";
      // Upload/camera buttons in search box (SVG icons)
      if (has("XDyW0e")) sv["display"] = "none";
      if (has("iblpc")) sv["display"] = "none";
    }
  }
  // input[type=hidden] — never visible regardless of stylesheet.
  // (The UA stylesheet already has `input[type=hidden] { display: none }`,
  // but case-insensitive matching of HTML attribute values isn't
  // guaranteed by our selector engine, so keep this as a belt-and-braces
  // override.)
  if (tag == "input" && node->node) {
    auto type_it = node->node->attributes.find("type");
    if (type_it != node->node->attributes.end()) {
      std::string itype = lower(type_it->second);
      if (itype == "hidden") sv["display"] = "none";
    }
  }
  // Elements with HTML `hidden` attribute — treat as display:none
  if (node->node && node->node->attributes.count("hidden")) {
    sv["display"] = "none";
  }
}
