# Browser Engine — Feature Implementation Plan

## Architecture Overview

```
Build Types:
  - rebuild.ps1  → Recompiles src/*.cpp only, links with pre-built third-party .o files (~30s)
  - build.ps1    → Full rebuild: lexbor + quickjs + lunasvg + plutovg + browser (~3-5 min)

Third-Party Libraries (require full rebuild if changed):
  - lexbor        → HTML5 parser (C)
  - quickjs       → ES2020 JavaScript engine (C)
  - lunasvg       → SVG rasterizer (C++)
  - plutovg       → Vector graphics backend for lunasvg (C)
  - GDI+          → Image decoding (Windows built-in)
  - WinINet       → HTTP/HTTPS networking (Windows built-in)

Browser Sources (incremental rebuild):
  src/main.cpp, layout.cpp, layout_inline.cpp, paint.cpp, renderer.cpp,
  css_parser.cpp, css_values.cpp, style.cpp, selector.cpp, ua_styles.cpp,
  dom.cpp, html_parser.cpp, lexbor_adapter.cpp, page_loader.cpp, net.cpp,
  image_cache.cpp, svg_rasterizer.cpp, font_metrics.cpp, color_utils.cpp,
  browser_ui.cpp, browser_tabs.cpp, browser_address.cpp, js.cpp,
  quickjs_adapter.cpp
```

---

## PHASE 1 — Foundation & Core Fixes (Incremental Rebuild Only)

**Goal:** Fix existing bugs and add foundational features that everything else depends on.
**Build:** `rebuild.ps1` only — no new libraries needed.
**Estimated scope:** ~2,500 lines across 8 files.

### 1.1 — CSS `text-transform` Property
**Files:** `css_values.cpp`, `paint.cpp`
**What:**
- Parse `text-transform: uppercase | lowercase | capitalize | none`
- Apply transform during display list text generation in `build_display_list()`
- Uppercase: convert all chars to upper via `towupper()`
- Lowercase: convert all chars to lower via `towlower()`
- Capitalize: uppercase first char of each word

**Test cases:**
- `<div style="text-transform:uppercase">hello world</div>` → "HELLO WORLD"
- `<div style="text-transform:capitalize">hello world</div>` → "Hello World"
- Nested elements inherit text-transform
- Unicode characters (accented letters)

---

### 1.2 — CSS `text-indent` Property
**Files:** `css_values.cpp`, `layout_inline.cpp`
**What:**
- Parse `text-indent: <length> | <percentage>`
- In `layout_inline_flow()`, add indent to first line's cursor_x
- Only applies to the FIRST line of a block container

**Test cases:**
- `text-indent: 40px` → first line starts 40px from left
- `text-indent: 10%` → 10% of containing block width
- Negative values (hanging indent)
- Doesn't apply to inline elements

---

### 1.3 — CSS `outline-offset` Property
**Files:** `paint.cpp`, `renderer.cpp`
**What:**
- Parse `outline-offset: <length>` (default 0)
- In outline rendering, offset the outline rect by this value
- Positive = further from border, negative = overlaps border

---

### 1.4 — CSS `visibility: hidden`
**Files:** `paint.cpp`
**What:**
- Currently `visibility: hidden` is not handled (only `display: none` hides elements)
- In `build_display_list()`, skip painting commands for `visibility: hidden` elements
- Element still occupies space (unlike `display: none`)
- Children inherit but can override with `visibility: visible`

**Test cases:**
- Hidden element takes up space but is invisible
- Child with `visibility: visible` inside hidden parent IS visible
- Text nodes inside hidden elements are not painted

---

### 1.5 — Missing CSS Pseudo-classes
**Files:** `selector.cpp`, `style.cpp`
**What:**
- `:empty` — matches elements with no children (or only whitespace text)
- `:only-child` — matches elements that are the sole child
- `:first-of-type` / `:last-of-type` — matches first/last element of its tag among siblings
- `:nth-of-type(An+B)` / `:nth-last-child(An+B)` — type-aware nth matching
- `:disabled` / `:enabled` — for form controls (check `disabled` attribute)
- `:checked` — for `<input type="checkbox|radio">` with `checked` attribute

**Implementation:**
- Add new PseudoClassType enum values
- Parse in `parse_simple_selector()`
- Match in `selector_matches()` by checking DOM tree relationships

**Test cases:**
- `li:first-of-type` matches first `<li>` in a `<ul>`
- `input:disabled` matches `<input disabled>`
- `div:empty` matches `<div></div>` but not `<div> </div>` (spec says whitespace doesn't count for some pseudo-classes, but does for :empty — verify)
- `:nth-of-type(2n)` matches every even element of its type

---

### 1.6 — CSS `list-style-type` Rendering
**Files:** `paint.cpp`, `layout.cpp`
**What:**
- Render bullet markers for `<ul>` items: disc (filled circle), circle (hollow), square
- Render number markers for `<ol>` items: decimal, lower-alpha, upper-alpha, lower-roman, upper-roman
- Position markers in the margin area (outside the content box)
- `list-style-type: none` suppresses markers

**Implementation:**
- In `build_display_list()`, when processing `<li>` elements:
  - Determine parent (`<ul>` or `<ol>`)
  - Calculate item index among siblings
  - Generate marker text/symbol
  - Position marker at `content.x - marker_width - gap`
- Disc: render filled circle (or use bullet char U+2022)
- Numbers: convert index to appropriate format

**Test cases:**
- `<ul>` shows disc bullets by default
- `<ol>` shows decimal numbers
- `list-style-type: none` hides markers
- Nested lists change marker style (disc → circle → square)
- `<ol start="5">` starts numbering at 5

---

### 1.7 — `background-size` and `background-position`
**Files:** `paint.cpp`, `renderer.cpp`
**What:**
- `background-size: cover | contain | <length> | <percentage> | auto`
- `background-position: <x> <y>` (keywords: left/center/right top/center/bottom, or lengths)
- `background-repeat: repeat | no-repeat | repeat-x | repeat-y`
- These are critical for many websites that use CSS background images

**Implementation:**
- Parse values in `build_display_list()` when generating SolidColor commands with `bg_image`
- In renderer, when blitting background images:
  - `cover`: scale image to cover entire element (may crop)
  - `contain`: scale to fit within element (may letterbox)
  - Position: offset the image within the element rect
  - Repeat: tile the image if repeat is enabled

**Test cases:**
- `background-size: cover` on a div with a small image
- `background-position: center center` centers the image
- `background-repeat: no-repeat` shows image once
- `background-size: 50% 50%` scales to half the element

---

### 1.8 — `object-fit` for Images
**Files:** `layout.cpp`, `renderer.cpp`
**What:**
- `object-fit: fill | contain | cover | none | scale-down`
- Controls how `<img>` content fits within its box
- `fill` (default): stretches to fill
- `contain`: scales down preserving aspect ratio
- `cover`: scales up preserving aspect ratio, crops overflow

**Implementation:**
- Store object-fit value in display command or layout box
- In image blitting pass of renderer, adjust source/dest rects based on fit mode

---

### 1.9 — `overflow: hidden` Clipping Fix & `overflow-x`/`overflow-y`
**Files:** `paint.cpp`, `layout.cpp`
**What:**
- Currently overflow:hidden generates clip commands but may not handle all edge cases
- Add `overflow-x` / `overflow-y` separate axis handling
- Ensure `overflow: hidden` properly clips child content (both paint and hit-test)
- `overflow: visible` (default) allows content to overflow without clipping

**Test cases:**
- A div with `overflow: hidden` and a tall child: child content is clipped
- `overflow-x: hidden; overflow-y: scroll` allows vertical scroll but clips horizontal

---

## PHASE 2 — Table Layout & CSS Float (Incremental Rebuild Only)

**Goal:** Two major layout features that many websites depend on.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,800 lines across 3-4 files.

### 2.1 — CSS Table Layout
**Files:** `layout.cpp` (new section), `ua_styles.cpp`
**What:**
Full table layout algorithm per CSS 2.1 spec:

**Step 1: Data structures**
- `TableGrid` struct: 2D grid of cells, column widths, row heights
- Each cell tracks: row/col span, min-width, max-width, content

**Step 2: UA default styles**
- `table` → `display: table; border-collapse: separate; border-spacing: 2px`
- `tr` → `display: table-row`
- `td`, `th` → `display: table-cell; padding: 1px; vertical-align: middle`
- `th` → also `font-weight: bold; text-align: center`
- `thead`, `tbody`, `tfoot` → `display: table-row-group`
- `caption` → `display: table-caption`

**Step 3: Column width calculation (fixed table layout)**
- Pass 1: Calculate min/max intrinsic widths for each cell
- Pass 2: Distribute available width across columns
  - Explicit widths honored first
  - Remaining space distributed proportionally to max-width
  - Minimum widths are floor values
- Handle `colspan` by distributing across spanned columns

**Step 4: Row height calculation**
- For each row, height = max(cell heights in that row)
- Handle `rowspan` by distributing extra height across spanned rows
- `vertical-align` in cells: top/middle/bottom/baseline

**Step 5: Painting**
- `border-collapse: collapse` — adjacent cells share borders, thicker border wins
- `border-collapse: separate` — each cell has its own borders with `border-spacing` gap
- Cell backgrounds painted behind content

**Test cases:**
- Simple 3x3 table renders with aligned columns
- `colspan="2"` spans two columns
- `rowspan="2"` spans two rows
- `width: 100%` table fills container
- `border-collapse: collapse` merges borders
- `text-align` and `vertical-align` in cells
- Nested tables
- `<th>` renders bold and centered
- Empty cells still occupy space

---

### 2.2 — CSS Float Layout
**Files:** `layout.cpp`, `layout_inline.cpp`
**What:**
Implement `float: left | right` and `clear: left | right | both`.

**Data structures:**
- Per-block float context: list of active floats (rect + side)
- Each float record: `{ x, y, width, height, side }`

**Algorithm:**
1. When a child has `float: left`:
   - Remove from normal flow
   - Place at the current left edge, below any existing left floats at that Y
   - If it doesn't fit beside existing floats, move down until it fits
   - Record the float rect in the parent's float context

2. When a child has `float: right`:
   - Same but positioned from the right edge

3. For non-floated block children:
   - Available width = container width minus active floats at current Y
   - Content wraps around floats

4. For inline content (text wrapping):
   - Each line's available width is reduced by floats that overlap that line's Y range
   - Line start X may be pushed right by left floats
   - Line end X may be pushed left by right floats

5. `clear: left` — move below all left floats
   `clear: right` — move below all right floats
   `clear: both` — move below all floats

6. Container height:
   - Normally determined by non-floated content
   - If container has `overflow: hidden` or is a BFC, it expands to contain floats

**Test cases:**
- `<img style="float:left">` with text wrapping around it
- Multiple left floats stack horizontally then wrap
- `clear: both` on an element pushes it below all floats
- Float inside a container with `overflow: hidden` (BFC)
- Float wider than container wraps to next line
- Negative margins on floats
- Floated elements respect padding of parent

---

## PHASE 3 — Browser Features (Incremental Rebuild Only)

**Goal:** User-facing browser features that improve usability.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,500 lines across 3 files.

### 3.1 — Text Selection & Copy
**Files:** `main.cpp`, `renderer.cpp`, `paint.cpp`
**What:**

**Mouse-based selection:**
- On `WM_LBUTTONDOWN`: record start position (X, Y in content coordinates)
- On `WM_MOUSEMOVE` (while button held): update end position, mark selection dirty
- On `WM_LBUTTONUP`: finalize selection
- Double-click: select word under cursor
- Triple-click: select entire line/paragraph

**Hit-testing for text:**
- Walk display list to find Text commands at click position
- For each Text command: use `FontMetrics::measure_text()` to find character offset
- Store selection as: `{ start_node, start_offset, end_node, end_offset }`

**Rendering selection:**
- In paint pass, for each text command that overlaps selection range:
  - Calculate selected substring pixel range
  - Draw blue highlight rect behind selected text
  - Draw selected text in white color

**Copy to clipboard:**
- On `Ctrl+C`: extract selected text from DOM nodes
- Use `OpenClipboard()`, `SetClipboardData(CF_UNICODETEXT, ...)`, `CloseClipboard()`

**Test cases:**
- Click-drag selects text across elements
- Ctrl+C copies selected text
- Double-click selects a word
- Selection highlights render correctly
- Selection across multiple lines/elements
- Clicking elsewhere deselects

---

### 3.2 — Find in Page (Ctrl+F)
**Files:** `main.cpp`, `renderer.cpp`, `browser_ui.cpp`
**What:**

**UI:**
- `Ctrl+F` opens find bar at top of content area (or bottom)
- Text input field + "Previous" / "Next" buttons + match count display
- `Escape` closes find bar
- `Enter` = next match, `Shift+Enter` = previous match

**Search:**
- Extract all text content from DOM tree into a flat string with node/offset mappings
- Search for query string (case-insensitive by default)
- Store all match positions as `{ node, start_offset, end_offset }` pairs

**Highlighting:**
- Current match: orange/yellow background
- Other matches: light yellow background
- Scroll to current match position
- Cycle through matches with Next/Previous

**Test cases:**
- Ctrl+F opens find bar
- Typing shows match count
- Enter cycles through matches
- Matches highlighted in content
- Escape closes find bar
- Case-insensitive matching

---

### 3.3 — Zoom (Ctrl+/Ctrl-)
**Files:** `main.cpp`, `renderer.cpp`, `layout.cpp`
**What:**

**Zoom levels:** 25%, 33%, 50%, 67%, 75%, 80%, 90%, 100%, 110%, 125%, 150%, 175%, 200%, 250%, 300%
**Default:** 100%

**Implementation approach — CSS pixel scaling:**
- Store zoom factor as global `g_zoom_level` (float, default 1.0)
- On zoom change:
  - Multiply all `parse_px()` results by zoom factor
  - Re-layout with adjusted viewport width (`buffer_width / zoom`)
  - Rebuild display list
  - In renderer, all pixel coordinates are already in CSS pixels × zoom
- `Ctrl+=` zoom in, `Ctrl+-` zoom out, `Ctrl+0` reset to 100%
- `Ctrl+mousewheel` also zooms

**Test cases:**
- Ctrl++ increases text and layout size
- Ctrl+- decreases
- Ctrl+0 resets
- Layout reflows correctly at different zoom levels
- Scrollbar adjusts for zoomed content height

---

### 3.4 — Context Menu (Right-Click)
**Files:** `main.cpp`
**What:**

**Menu items (context-dependent):**
- On text: Copy, Select All
- On link: Open Link, Copy Link Address
- On image: Open Image, Copy Image Address
- On page: Reload, View Page Source, Select All
- On input field: Cut, Copy, Paste, Select All

**Implementation:**
- `WM_RBUTTONDOWN`: hit-test to determine what's under cursor
- `CreatePopupMenu()` + `AppendMenuA()` for each item
- `TrackPopupMenu()` to show
- `WM_COMMAND` to handle selection
- Each action calls existing functions (navigate_to, clipboard ops, etc.)

---

### 3.5 — View Page Source
**Files:** `main.cpp`, `page_loader.cpp`
**What:**
- Store raw HTML source during page load (before parsing)
- "View Page Source" opens a new tab with syntax-highlighted source
- Or display in a simple modal/text window
- Keywords colored: tags (blue), attributes (green), values (red), comments (gray)

---

## PHASE 4 — Networking & Forms (Incremental Rebuild Only)

**Goal:** Make forms work and improve networking for real-world usage.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~800 lines across 3 files.

### 4.1 — HTTP POST Support
**Files:** `net.cpp`
**What:**
- Extend `fetch_https()` to accept HTTP method + body + content-type
- Switch from `InternetOpenUrlA()` to `HttpOpenRequestA()` + `HttpSendRequestA()`
- Support `Content-Type: application/x-www-form-urlencoded` and `multipart/form-data`
- Return response headers alongside body

**API change:**
```cpp
struct HttpResponse {
    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
};
HttpResponse http_request(const std::string& url, const std::string& method,
    const std::string& body, const std::map<std::string, std::string>& headers);
```

---

### 4.2 — Cookie Storage
**Files:** `net.cpp` (new cookie_store section)
**What:**
- Parse `Set-Cookie` response headers
- Store cookies per domain: `{ name, value, domain, path, expires, httpOnly, secure }`
- Send matching cookies on subsequent requests via `Cookie:` header
- In-memory store (no persistence to disk initially)
- Respect `path`, `domain`, `secure`, `httpOnly` attributes
- Expiration handling (discard expired cookies)

---

### 4.3 — Form Submission
**Files:** `main.cpp`, `page_loader.cpp`, `net.cpp`
**What:**

**GET form submission:**
- Collect all form controls (`<input>`, `<select>`, `<textarea>`) within the `<form>`
- URL-encode name=value pairs
- Append as query string to form's `action` URL
- Navigate to the resulting URL

**POST form submission:**
- Same collection of form controls
- Encode as `application/x-www-form-urlencoded` body
- Send POST request to form's `action` URL
- Navigate to the response

**Form controls to support:**
- `<input type="text|search|email|tel|url|number|password|hidden">` — use current value
- `<input type="checkbox|radio">` — include only if checked
- `<input type="submit">` — include the clicked submit button's name/value
- `<select>` — include selected option's value
- `<textarea>` — include inner text

**Test cases:**
- Google search form submission (GET)
- Login form (POST with username/password)
- Checkbox/radio selection
- Hidden inputs included
- Multiple submit buttons (only clicked one sent)

---

### 4.4 — Custom Request Headers
**Files:** `net.cpp`
**What:**
- Accept header map in `http_request()`
- `HttpAddRequestHeaders()` for each custom header
- Standard headers to always send: `Accept`, `Accept-Language`, `Accept-Encoding`
- Referer header: send previous page URL

---

## PHASE 5 — CSS Animations & Transitions (Incremental Rebuild Only)

**Goal:** Bring pages to life with motion.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,200 lines across 4 files.

### 5.1 — CSS Transitions
**Files:** `style.cpp`, `layout.cpp`, `main.cpp`
**What:**

**Properties:**
- `transition-property` — which CSS properties to animate
- `transition-duration` — how long (e.g., `0.3s`, `200ms`)
- `transition-timing-function` — `linear`, `ease`, `ease-in`, `ease-out`, `ease-in-out`, `cubic-bezier()`
- `transition-delay` — delay before start

**Implementation:**
- On style change (hover, class change, JS):
  - Compare old vs new computed values for transitioned properties
  - If different, create a TransitionState: `{ property, start_value, end_value, start_time, duration, easing }`
  - Store active transitions per element
- On each repaint (timer-driven):
  - For each active transition, calculate current interpolated value based on elapsed time
  - Apply interpolated value as computed style
  - Re-layout and repaint
  - Remove transition when complete

**Interpolatable properties:**
- Numeric: `width`, `height`, `margin-*`, `padding-*`, `top/left/right/bottom`, `font-size`, `border-width`, `opacity`, `border-radius`
- Color: `color`, `background-color`, `border-color` (interpolate R/G/B/A channels)
- Transform: `translate`, `scale`, `rotate` (interpolate individual values)

**Timing functions (cubic bezier):**
- `linear` = `cubic-bezier(0, 0, 1, 1)`
- `ease` = `cubic-bezier(0.25, 0.1, 0.25, 1)`
- `ease-in` = `cubic-bezier(0.42, 0, 1, 1)`
- `ease-out` = `cubic-bezier(0, 0, 0.58, 1)`
- `ease-in-out` = `cubic-bezier(0.42, 0, 0.58, 1)`

---

### 5.2 — CSS Animations (@keyframes)
**Files:** `css_parser.cpp`, `style.cpp`, `main.cpp`
**What:**

**Parsing:**
- `@keyframes name { from { ... } 50% { ... } to { ... } }`
- Store as named animation with percentage-keyed property maps

**Properties:**
- `animation-name` — reference to @keyframes
- `animation-duration` — total duration
- `animation-timing-function` — per-keyframe or overall easing
- `animation-delay` — delay before start
- `animation-iteration-count` — number or `infinite`
- `animation-direction` — `normal`, `reverse`, `alternate`, `alternate-reverse`
- `animation-fill-mode` — `none`, `forwards`, `backwards`, `both`
- `animation-play-state` — `running`, `paused`

**Implementation:**
- On element creation/style application, start animations
- Each frame: calculate progress, find surrounding keyframes, interpolate
- Same interpolation as transitions but with multiple keyframes
- Timer-driven repaint loop (16ms for ~60fps)

---

### 5.3 — `requestAnimationFrame` Support
**Files:** `quickjs_adapter.cpp`, `main.cpp`
**What:**
- Already partially mapped in QuickJS adapter
- Needs proper frame-based scheduling (not setTimeout)
- Add 16ms timer that fires rAF callbacks
- Provide `DOMHighResTimeStamp` argument
- Cancel with `cancelAnimationFrame()`

---

## PHASE 6 — Image Formats & Rendering Quality (Full Rebuild Required)

**Goal:** Support modern image formats and improve rendering quality.
**Build:** `build.ps1` (full rebuild) — new third-party libraries needed.
**Estimated scope:** ~600 lines + library integration.

### 6.1 — WebP Image Support
**New dependency:** libwebp (https://chromium.googlesource.com/webm/libwebp)
**Files:** `image_cache.cpp`, `build.ps1`
**What:**
- Download and integrate libwebp source (decode only: ~20 files)
- Detect WebP format by magic bytes (`RIFF....WEBP`)
- Decode WebP → RGBA pixels → convert to BGRA for GDI buffer
- Animated WebP: decode first frame (no animation support initially)

**Build changes:**
- Add libwebp source files to `build.ps1` compilation step
- Add include path for webp headers
- Link resulting .o files

---

### 6.2 — AVIF Image Support (Optional)
**New dependency:** libavif + dav1d (AV1 decoder)
**Complexity:** High — dav1d is a large library
**Alternative:** Skip AVIF for now; most sites provide JPEG/PNG fallbacks
**Decision:** Defer to a later phase unless specifically needed

---

### 6.3 — Improved Text Rendering (DirectWrite)
**Files:** `font_metrics.cpp`, `renderer.cpp`, `build.ps1`
**What:**
- Replace GDI `TextOutA()` with DirectWrite for text rendering
- Benefits: ClearType anti-aliasing, subpixel positioning, better Unicode coverage
- Use `IDWriteFactory`, `IDWriteTextFormat`, `IDWriteTextLayout`
- Render text to a bitmap target, then composite into pixel buffer

**Alternative (simpler):**
- Keep GDI but enable ClearType: `SetProcessDPIAware()` + quality font creation flags
- `CreateFontA()` with `CLEARTYPE_QUALITY` instead of `DEFAULT_QUALITY`
- This gives better text rendering without DirectWrite complexity

---

### 6.4 — Animated GIF Support
**Files:** `image_cache.cpp`, `renderer.cpp`, `main.cpp`
**What:**
- GDI+ already decodes GIF frames via `FrameDimensionTime`
- Store all frames + delays in cache
- Add frame index + last_frame_time to CachedImage
- On timer tick, advance frame index, repaint

---

## PHASE 7 — JavaScript API Extensions (Incremental Rebuild Only)

**Goal:** Enable modern JS-driven websites.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,000 lines in `quickjs_adapter.cpp` + `net.cpp`.

### 7.1 — Fetch API
**Files:** `quickjs_adapter.cpp`, `net.cpp`
**What:**
- Implement `window.fetch(url, options)` returning a Promise
- Response object: `.status`, `.ok`, `.text()`, `.json()`, `.headers`
- Support GET and POST methods
- Execute network request in background thread
- Resolve Promise on main thread via pending timer mechanism

**API surface:**
```js
fetch('/api/data', { method: 'POST', body: JSON.stringify({key: 'value'}),
    headers: { 'Content-Type': 'application/json' } })
  .then(r => r.json())
  .then(data => console.log(data));
```

---

### 7.2 — `getComputedStyle()`
**Files:** `quickjs_adapter.cpp`, `style.cpp`
**What:**
- `window.getComputedStyle(element)` returns a CSSStyleDeclaration-like object
- Read specified_values from the element's StyledNode
- For properties not explicitly set, return inherited/default values
- Computed values should resolve relative units to px

---

### 7.3 — `classList` Full API
**Files:** `quickjs_adapter.cpp`
**What:**
- Currently `classList.add()` is a no-op to prevent JS breaking layout
- Properly implement:
  - `classList.add(cls)` — add class, mark DOM dirty
  - `classList.remove(cls)` — remove class, mark DOM dirty
  - `classList.toggle(cls)` — toggle class
  - `classList.contains(cls)` — check presence
  - `classList.replace(old, new)` — swap classes
- After class change: re-match CSS rules, re-layout, re-paint

---

### 7.4 — DOM Manipulation Improvements
**Files:** `quickjs_adapter.cpp`
**What:**
- `element.insertAdjacentHTML(position, html)` — parse HTML string, insert at position
- `element.closest(selector)` — walk up ancestors matching selector
- `element.matches(selector)` — check if element matches CSS selector
- `element.nextElementSibling` / `previousElementSibling` — skip text nodes
- `element.children` — element-only child list (skip text nodes)
- `element.childElementCount` — number of element children
- `document.createDocumentFragment()` — lightweight container for batch DOM ops

---

### 7.5 — Event System Improvements
**Files:** `quickjs_adapter.cpp`, `main.cpp`
**What:**
- Event bubbling: events propagate up from target to document
- `event.stopPropagation()` — stop bubbling
- `event.preventDefault()` — prevent default behavior
- `event.target` — the original target element
- `event.currentTarget` — the element the listener is on
- Event capturing phase (addEventListener 3rd arg `{ capture: true }`)
- `DOMContentLoaded` event fired after DOM + CSS parsed
- `load` event fired after all resources loaded

---

## PHASE 8 — Advanced Layout Features (Incremental Rebuild Only)

**Goal:** Handle more complex page layouts.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,500 lines across 2-3 files.

### 8.1 — CSS Multi-Column Layout
**Files:** `layout.cpp`
**What:**
- `column-count: <number>` — split content into N columns
- `column-width: <length>` — ideal column width (browser calculates count)
- `column-gap: <length>` — gap between columns
- `column-rule: <width> <style> <color>` — line between columns
- `column-span: all` — element spans all columns (e.g., headings)

**Algorithm:**
- Calculate actual column count from column-count/column-width/available width
- Layout content in first column until height threshold
- Overflow into next column
- Balance column heights (approximately equal)

---

### 8.2 — CSS Grid Layout (Basic)
**Files:** `layout.cpp`
**What:**
A basic grid implementation covering the most common patterns:

**Properties:**
- `display: grid`
- `grid-template-columns: <track-list>` (px, fr, auto, repeat())
- `grid-template-rows: <track-list>`
- `gap` / `row-gap` / `column-gap`
- `grid-column: <start> / <end>` (line numbers)
- `grid-row: <start> / <end>`
- `justify-items` / `align-items`

**Algorithm:**
1. Parse track definitions into column/row sizes
2. Handle `fr` units: distribute remaining space proportionally
3. Handle `auto`: size to content
4. Handle `repeat(N, ...)`: expand into N copies
5. Place items: explicit placement (grid-column/grid-row) first, then auto-placement
6. Size tracks: resolve auto tracks based on content
7. Position items: calculate x/y from track positions + gaps

**Not in scope (defer to later):**
- `grid-template-areas` (named areas)
- `grid-auto-flow: dense` (complex packing)
- Subgrid
- `minmax()` function

---

### 8.3 — Flex `order` and `align-self`
**Files:** `layout.cpp`
**What:**
- `order: <integer>` — reorder flex children visually (default 0)
  - Sort children by order value before layout
  - DOM order unchanged, only visual order
- `align-self: auto | flex-start | flex-end | center | stretch | baseline`
  - Override parent's `align-items` for this specific child
  - Applied during cross-axis positioning

---

### 8.4 — `min-content` / `max-content` / `fit-content` Sizing
**Files:** `layout.cpp`, `css_values.cpp`
**What:**
- `width: min-content` — shrink to narrowest possible without overflow
- `width: max-content` — expand to widest without wrapping
- `width: fit-content` — `min(max-content, max(min-content, available))`
- Requires two-pass layout: first pass to determine intrinsic sizes, second pass to apply

---

## PHASE 9 — `<iframe>`, `<canvas>`, `<video>` (Incremental Rebuild)

**Goal:** Support embedded content.
**Build:** `rebuild.ps1` only (except video codecs).
**Estimated scope:** ~2,000 lines across multiple files.

### 9.1 — `<iframe>` Support
**Files:** `layout.cpp`, `page_loader.cpp`, `renderer.cpp`, `main.cpp`
**What:**
- Parse `<iframe src="url" width="N" height="N">`
- Fetch iframe URL in background thread
- Parse HTML/CSS, build separate style tree + layout tree
- Render iframe content into a sub-buffer
- Blit sub-buffer into parent page at iframe position
- Scroll independently from parent
- Each iframe has its own JS context (sandboxed)
- Limit: max 3 nested iframes to prevent infinite recursion

**Limitations (acceptable for v1):**
- No cross-origin communication (postMessage)
- No sandbox attribute
- No srcdoc attribute

---

### 9.2 — `<canvas>` 2D Context (Basic)
**Files:** new `canvas.cpp` + `canvas.h`, `quickjs_adapter.cpp`
**What:**
A software-rendered Canvas 2D API:

**Methods:**
- `getContext('2d')` — returns CanvasRenderingContext2D
- `fillRect(x, y, w, h)`, `strokeRect(x, y, w, h)`, `clearRect(x, y, w, h)`
- `fillText(text, x, y)`, `strokeText(text, x, y)`
- `beginPath()`, `moveTo()`, `lineTo()`, `closePath()`, `fill()`, `stroke()`
- `arc(x, y, r, startAngle, endAngle)` — circle/arc drawing
- `drawImage(img, x, y, w, h)` — draw image onto canvas
- `save()`, `restore()` — context state stack

**Properties:**
- `fillStyle`, `strokeStyle` — color strings
- `lineWidth`, `lineCap`, `lineJoin`
- `font` — text font
- `textAlign`, `textBaseline`
- `globalAlpha` — opacity

**Implementation:**
- Canvas element gets its own pixel buffer (width × height × 4 bytes)
- Drawing operations write directly to this buffer
- Buffer is blitted to screen during render pass (like image)
- JS bindings expose each method via QuickJS

---

### 9.3 — `<video>` Element (Placeholder)
**Files:** `layout.cpp`, `paint.cpp`
**What (minimal v1):**
- Render video poster image if `poster` attribute is set
- Display play button overlay
- Show fallback text if no poster
- No actual video playback (would require FFmpeg or Media Foundation — entire project on its own)

---

## PHASE 10 — Performance & Polish (Incremental Rebuild Only)

**Goal:** Make the browser faster and more polished.
**Build:** `rebuild.ps1` only.

### 10.1 — Incremental Layout
**What:**
- Track which subtrees changed (DOM dirty flag per node)
- Only re-layout dirty subtrees, not the entire page
- Cache computed styles, only recompute on class/style change
- Major perf improvement for JS-driven updates

### 10.2 — Display List Caching
**What:**
- Cache display commands per layout box
- Only rebuild commands for dirty boxes
- Skip re-sorting entire display list on each paint

### 10.3 — Image Lazy Loading
**What:**
- `<img loading="lazy">` — defer loading until near viewport
- Track scroll position, load images when within 2x viewport height
- Reduces initial page load time and memory

### 10.4 — Session Restore
**What:**
- On exit, save open tabs (URL, title, scroll position) to JSON file
- On start, restore previous session
- File: `~/.scratch_browser/session.json`

### 10.5 — Bookmarks
**What:**
- Star icon in address bar to bookmark current page
- Bookmarks stored in `~/.scratch_browser/bookmarks.json`
- Bookmarks bar below address bar (optional, toggle with Ctrl+Shift+B)
- Bookmarks menu in context menu

### 10.6 — History
**What:**
- Store visited URLs with timestamps in `~/.scratch_browser/history.json`
- Ctrl+H opens history panel
- Address bar autocomplete from history
- Clear history option

---

## PHASE 11 — Web Fonts & Advanced CSS Selectors (Full Rebuild Required)

**Goal:** Support custom fonts and modern CSS selectors.
**Build:** `build.ps1` (full rebuild) — needs WOFF2 decoding library.
**Estimated scope:** ~1,800 lines + library integration.

### 11.1 — `@font-face` & Web Fonts
**Files:** `css_parser.cpp`, `page_loader.cpp`, `font_metrics.cpp`, `build.ps1`
**New dependency:** woff2 decoder (Google's brotli-based WOFF2 library, or stb_truetype for TTF/OTF)
**What:**

**Parsing:**
- `@font-face { font-family: "Name"; src: url("font.woff2") format("woff2"); font-weight: 400; font-style: normal; }`
- Store font declarations: family name → list of sources with weight/style variants

**Loading:**
- Fetch font files via `fetch_https()` during page load
- WOFF2: decompress brotli → TTF
- TTF/OTF: use directly
- Cache decoded fonts in memory by family+weight+style key

**Rendering:**
- Option A (simpler): Install font temporarily via `AddFontMemResourceEx()` (Windows API), then use with GDI `CreateFontA()`
- Option B (better): Integrate stb_truetype for direct glyph rasterization — no OS dependency
- Map CSS `font-family` to loaded font during style resolution
- Fallback chain: web font → system font → Arial

**Font matching:**
- Match `font-weight` (100-900) to closest available variant
- Match `font-style` (normal/italic/oblique) to available variant
- `font-display: swap` — render with fallback immediately, swap when loaded
- `font-display: block` — invisible text until loaded (3s timeout)

**Test cases:**
- Google Fonts loaded and rendered
- Bold/italic variants selected correctly
- Fallback to system font when web font fails
- WOFF2 format decoded correctly
- Multiple @font-face declarations for same family (weight variants)

---

### 11.2 — `@import` CSS
**Files:** `page_loader.cpp`, `css_parser.cpp`
**What:**
- `@import url("other.css");` — fetch and prepend imported stylesheet
- `@import url("other.css") screen;` — conditional import with media query
- Process imports BEFORE main CSS parsing
- Limit: max 5 levels of nesting to prevent infinite loops
- Limit: max 10 total imports

**Implementation:**
- In CSS preprocessing, scan for `@import` at the beginning of stylesheets
- Fetch each imported URL
- Recursively scan imported sheets for further imports
- Concatenate all imported CSS before the importing sheet
- Remove `@import` statements from final CSS

---

### 11.3 — Advanced CSS Selectors
**Files:** `selector.cpp`, `css_parser.cpp`
**What:**

**`:is()` / `:where()`:**
- `:is(h1, h2, h3)` — matches any of the listed selectors
- `:where()` — same but with zero specificity
- Parse comma-separated selector list inside parentheses
- Match if ANY selector in the list matches

**`:has()` (parent selector):**
- `div:has(> img)` — matches `<div>` that contains a direct child `<img>`
- `a:has(img)` — matches `<a>` containing `<img>` anywhere
- Walk descendants/children of candidate element to check inner selector
- Performance concern: limit depth of :has() traversal to 10 levels

**`:not()` improvements:**
- Current: single simple selector only
- Extend to: compound selectors, selector lists
- `:not(.a, .b)` — matches elements that are neither .a nor .b

**Test cases:**
- `:is(h1, h2, h3) { color: red }` applies to all three heading types
- `:where(.foo, .bar)` doesn't increase specificity
- `div:has(> .active)` matches parent div of .active element
- `:not(.a, .b)` excludes both classes

---

### 11.4 — CSS Nesting
**Files:** `css_parser.cpp`
**What:**
- Native CSS nesting (no preprocessor):
```css
.card {
  padding: 16px;
  & .title { font-weight: bold; }
  &:hover { background: #eee; }
  @media (max-width: 600px) { padding: 8px; }
}
```
- `&` refers to parent selector
- Nested rules are flattened during parsing
- `.card & .title` → `.card .title`
- `.card &:hover` → `.card:hover`
- Nested `@media` inherits parent selector context

**Implementation:**
- When encountering `{` inside a rule body that starts a new selector:
  - Push current selector onto a stack
  - Parse nested selector, prepend parent selector
  - Generate flattened rule
  - Pop selector stack when `}` closes nested block

---

## PHASE 12 — Advanced CSS Visual Properties (Incremental Rebuild)

**Goal:** Support advanced visual effects used by modern sites.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,200 lines across 3 files.

### 12.1 — Multiple Backgrounds
**Files:** `paint.cpp`, `css_parser.cpp`
**What:**
- `background: url(a.png) no-repeat top left, url(b.png) repeat bottom right, #fff`
- Multiple background layers separated by commas
- Each layer has its own: image, position, size, repeat, origin, clip
- Layers painted bottom to top (last listed = bottommost)
- Parse comma-separated background shorthand into vector of BackgroundLayer structs

---

### 12.2 — `border-image`
**Files:** `paint.cpp`, `renderer.cpp`
**What:**
- `border-image: url(border.png) 30 round`
- `border-image-source` — image URL
- `border-image-slice` — how to slice the image into 9 regions
- `border-image-repeat` — `stretch | repeat | round | space`
- Nine-patch rendering: corners fixed, edges stretched/repeated, center optional

---

### 12.3 — `clip-path`
**Files:** `paint.cpp`, `renderer.cpp`
**What:**
- `clip-path: circle(50%)` — circular clip
- `clip-path: ellipse(50% 40%)` — elliptical clip
- `clip-path: polygon(50% 0%, 0% 100%, 100% 100%)` — triangular clip
- `clip-path: inset(10px 20px 30px 40px round 5px)` — rounded rect clip
- Apply clip mask before painting element content
- Pixel-level: for each pixel, check if it's inside the clip shape

---

### 12.4 — `mix-blend-mode`
**Files:** `renderer.cpp`
**What:**
- `mix-blend-mode: multiply | screen | overlay | darken | lighten | color-dodge | color-burn | difference | exclusion | hue | saturation | color | luminosity`
- Applied when compositing element onto backdrop
- Per-pixel blend calculation between source and destination colors
- Each mode has a specific mathematical formula

**Implementation:**
- Store blend mode in DisplayCommand
- In pixel write functions, instead of simple overwrite, apply blend formula
- `multiply`: `result = src * dst / 255`
- `screen`: `result = src + dst - src * dst / 255`
- `overlay`: conditional multiply/screen based on dst value

---

### 12.5 — `backdrop-filter`
**Files:** `renderer.cpp`, `paint.cpp`
**What:**
- `backdrop-filter: blur(10px)` — blur the content BEHIND the element
- `backdrop-filter: brightness(0.5) blur(5px)` — chain multiple filters
- Read pixels from buffer behind element rect, apply filters, write back, then paint element on top

**Implementation:**
- After painting all content below this element's z-order:
  - Read pixel region from buffer matching element rect
  - Apply filter chain (blur, brightness, etc.) to copied pixels
  - Write filtered pixels back to buffer
  - Then paint element's own content (usually semi-transparent)

---

### 12.6 — `mask` / `mask-image`
**Files:** `paint.cpp`, `renderer.cpp`
**What:**
- `mask-image: url(mask.png)` — use image alpha as mask
- `mask-image: linear-gradient(to bottom, black, transparent)` — gradient mask
- After painting element, multiply each pixel's alpha by mask's luminance/alpha
- Pixels where mask is black → fully visible; where mask is transparent → hidden

---

### 12.7 — `aspect-ratio` Property
**Files:** `layout.cpp`, `css_values.cpp`
**What:**
- `aspect-ratio: 16 / 9` — maintain width/height ratio
- If width is set and height is auto: `height = width / ratio`
- If height is set and width is auto: `width = height * ratio`
- If both auto: use intrinsic size, then apply ratio
- Interacts with `min-width`/`max-width`/`min-height`/`max-height`

---

### 12.8 — `scroll-snap` Properties
**Files:** `main.cpp`, `layout.cpp`
**What:**
- `scroll-snap-type: y mandatory | y proximity | x mandatory`
- `scroll-snap-align: start | center | end`
- On scroll end, snap to nearest snap point
- Calculate snap points from child positions with `scroll-snap-align`
- Animate scroll to snap position (smooth snap)

---

## PHASE 13 — `@container` Queries & `@supports` (Incremental Rebuild)

**Goal:** Modern responsive design features.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~800 lines across 3 files.

### 13.1 — `@container` Queries
**Files:** `css_parser.cpp`, `page_loader.cpp`, `layout.cpp`, `style.cpp`
**What:**
- `container-type: inline-size | size` — mark element as a container
- `container-name: sidebar` — name the container
- `@container sidebar (min-width: 400px) { ... }` — conditional styles based on container size

**Implementation:**
- During style resolution, mark containers with `container-type`
- During layout, after computing container dimensions:
  - Evaluate `@container` conditions against container's computed size
  - Apply/remove rules based on match
  - May require a second layout pass for affected children

**Limitation:** Container queries create circular dependencies — container size depends on children, but children's styles depend on container size. Break cycle by computing container size first (from explicit width/max-width), then applying container queries.

---

### 13.2 — `@supports` Feature Queries (Proper Evaluation)
**Files:** `page_loader.cpp`, `css_parser.cpp`
**What:**
- Currently: `@supports` blocks are always included
- Fix: evaluate the condition to check if the property/value is actually supported
- `@supports (display: grid) { ... }` — include if grid is implemented
- `@supports not (display: grid) { ... }` — include if grid is NOT implemented
- `@supports (display: flex) and (gap: 10px) { ... }` — logical AND
- `@supports (display: flex) or (display: grid) { ... }` — logical OR

**Implementation:**
- Maintain a set of supported properties and values
- Parse `@supports` condition into boolean expression tree
- Evaluate against supported set
- Include or exclude rules accordingly

---

### 13.3 — `contain` / `content-visibility`
**Files:** `layout.cpp`, `paint.cpp`
**What:**
- `contain: layout` — element's layout doesn't affect outside (optimization hint)
- `contain: paint` — element's painting is clipped to its box
- `contain: size` — element's size isn't affected by children
- `contain: strict` — all containment types
- `content-visibility: auto` — skip rendering off-screen elements entirely
- `content-visibility: hidden` — like display:none but preserves layout state

**Performance benefit:**
- Elements with `content-visibility: auto` that are off-screen: skip layout + paint entirely
- Track visibility via scroll position intersection
- When scrolled into view, layout and paint on demand

---

## PHASE 14 — Writing Modes, BiDi & Internationalization (Incremental Rebuild)

**Goal:** Support right-to-left text, vertical writing, and international text input.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,500 lines across 5 files.

### 14.1 — `writing-mode` Property
**Files:** `layout.cpp`, `layout_inline.cpp`, `renderer.cpp`
**What:**
- `writing-mode: horizontal-tb` (default) — horizontal, top to bottom
- `writing-mode: vertical-rl` — vertical, right to left (Japanese/Chinese)
- `writing-mode: vertical-lr` — vertical, left to right (Mongolian)

**Implementation:**
- Swap inline/block directions based on writing mode
- `horizontal-tb`: inline = horizontal, block = vertical (current behavior)
- `vertical-rl`: inline = vertical, block = horizontal (right to left)
- Affects: text layout direction, line stacking direction, margin/padding logical mapping
- Text rendering: rotate glyphs 90° for vertical modes

---

### 14.2 — `direction` & BiDi Text
**Files:** `layout_inline.cpp`, `renderer.cpp`
**What:**
- `direction: ltr | rtl` — base text direction
- `unicode-bidi: normal | embed | bidi-override | isolate`
- Unicode Bidirectional Algorithm (UBA) for mixed LTR/RTL text

**Implementation (simplified):**
- Detect RTL characters (Arabic U+0600-U+06FF, Hebrew U+0590-U+05FF, etc.)
- For `direction: rtl`:
  - Reverse inline flow (start from right edge)
  - Text-align default becomes right
  - Margin/padding logical properties swap
- For mixed content:
  - Split text runs by directionality
  - Reorder runs per Unicode Bidi algorithm (simplified: reverse RTL runs within LTR context)

**Full UBA is extremely complex — implement simplified version first:**
- Level 1: Detect script direction per character
- Level 2: Group consecutive same-direction characters into runs
- Level 3: Reverse RTL runs within LTR paragraphs (and vice versa)
- Skip: explicit embedding levels, bracket pairing

---

### 14.3 — IME Input (CJK Languages)
**Files:** `main.cpp`
**What:**
- Handle `WM_IME_STARTCOMPOSITION`, `WM_IME_COMPOSITION`, `WM_IME_ENDCOMPOSITION`
- Display composition string (candidate text) near input cursor
- `ImmGetContext()` / `ImmGetCompositionString()` to read IME state
- Insert final composed text into focused input element

**Test cases:**
- Type Chinese pinyin in search bar → candidates appear → select → text inserted
- Japanese IME hiragana → kanji conversion
- Korean jamo composition

---

### 14.4 — CSS Logical Properties
**Files:** `css_values.cpp`, `layout.cpp`
**What:**
- `margin-inline-start` / `margin-inline-end` — direction-aware margins
- `padding-block-start` / `padding-block-end` — writing-mode-aware padding
- `border-inline-start` / `border-block-end` — direction-aware borders
- `inline-size` / `block-size` — width/height based on writing mode
- Map logical → physical based on `direction` and `writing-mode`:
  - LTR horizontal: inline-start = left, block-start = top
  - RTL horizontal: inline-start = right, block-start = top
  - Vertical-rl: inline-start = top, block-start = right

---

## PHASE 15 — Security & Standards Compliance (Incremental Rebuild)

**Goal:** Browser security features and standards compliance.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~1,200 lines across 4 files.

### 15.1 — CORS Enforcement
**Files:** `net.cpp`, `quickjs_adapter.cpp`
**What:**
- Track origin (scheme + host + port) of current page
- For JS-initiated requests (fetch/XHR):
  - Same-origin: allow freely
  - Cross-origin: check `Access-Control-Allow-Origin` response header
  - If header missing or doesn't match: block response from JS
- Preflight requests (`OPTIONS`) for non-simple requests
- `Access-Control-Allow-Methods`, `Access-Control-Allow-Headers` checking

---

### 15.2 — Content Security Policy (CSP)
**Files:** `page_loader.cpp`, `quickjs_adapter.cpp`
**What:**
- Parse `Content-Security-Policy` response header (or `<meta http-equiv="Content-Security-Policy">`)
- Directives: `script-src`, `style-src`, `img-src`, `connect-src`, `default-src`
- Values: `'self'`, `'unsafe-inline'`, `'unsafe-eval'`, specific domains, `'none'`
- Block resources that violate policy:
  - Inline `<script>` blocked if `script-src` doesn't include `'unsafe-inline'`
  - External scripts blocked if domain not in `script-src`
  - `eval()` blocked if `'unsafe-eval'` not present

---

### 15.3 — HTTPS Certificate UI
**Files:** `browser_ui.cpp`, `net.cpp`
**What:**
- Lock icon in address bar for HTTPS pages
- Warning icon for HTTP pages
- Certificate error page for invalid/expired certificates
- `InternetQueryOption()` with `INTERNET_OPTION_SECURITY_CERTIFICATE_STRUCT` to read cert info
- Display: issuer, expiry, domain match status

---

### 15.4 — `<meta http-equiv>` Handling
**Files:** `page_loader.cpp`
**What:**
- `<meta http-equiv="refresh" content="5;url=...">` — auto-redirect after delay
- `<meta http-equiv="Content-Type" content="text/html; charset=utf-8">` — encoding
- `<meta name="viewport" content="width=device-width, initial-scale=1">` — viewport sizing
- `<meta http-equiv="X-UA-Compatible" content="IE=edge">` — ignore (always modern mode)

**Viewport meta:**
- Parse `width=device-width` → use screen width
- Parse `initial-scale=1` → no zoom
- Parse `maximum-scale=1` → disable pinch zoom
- Affects `g_viewport_width` used for CSS media queries and layout

---

## PHASE 16 — Web Components & Modern HTML Elements (Incremental Rebuild)

**Goal:** Support Web Components and remaining HTML5 elements.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~2,000 lines across 5 files.

### 16.1 — `<details>` / `<summary>` Toggle
**Files:** `layout.cpp`, `main.cpp`, `ua_styles.cpp`, `paint.cpp`
**What:**
- `<details>` container: collapsed by default, shows only `<summary>` child
- Click `<summary>` → toggles `open` attribute → shows/hides remaining children
- Triangle marker (▶/▼) rendered before summary text
- `<details open>` starts expanded

**Implementation:**
- UA style: `details > :not(summary) { display: none }` when not `[open]`
- On click: toggle `open` attribute, re-match styles, re-layout
- Paint triangle marker before summary text (U+25B6 ▶ / U+25BC ▼)

---

### 16.2 — `<dialog>` Element
**Files:** `layout.cpp`, `main.cpp`, `ua_styles.cpp`, `quickjs_adapter.cpp`
**What:**
- `<dialog>` — hidden by default
- `dialog.showModal()` — show as modal with backdrop
- `dialog.show()` — show as non-modal
- `dialog.close(returnValue)` — close dialog
- `::backdrop` pseudo-element — semi-transparent overlay behind modal
- Focus trapping inside modal dialog
- Escape key closes modal

**Implementation:**
- UA style: `dialog { display: none }` / `dialog[open] { display: block }`
- JS API: `showModal()` sets `open` attribute + adds to top layer
- `::backdrop`: paint full-viewport semi-transparent rect behind dialog
- Position: centered in viewport (fixed positioning)

---

### 16.3 — `<progress>` / `<meter>` Elements
**Files:** `paint.cpp`, `layout.cpp`, `ua_styles.cpp`
**What:**
- `<progress value="0.7">` — progress bar (0 to 1 or 0 to max)
- `<meter value="0.7" min="0" max="1" low="0.3" high="0.8">` — gauge
- Render as styled bar: background track + filled portion
- Indeterminate progress (no `value`): animated stripe pattern

**Rendering:**
- Track: gray rounded rect
- Fill: green/blue rounded rect, width = value/max * element_width
- Meter zones: green (optimum), yellow (suboptimal), red (poor)

---

### 16.4 — `<select>` Dropdown Rendering
**Files:** `main.cpp`, `paint.cpp`, `layout.cpp`
**What:**
- Render `<select>` as a dropdown button showing selected option text
- Click opens dropdown list of `<option>` elements
- Arrow keys navigate options, Enter selects, Escape closes
- `<optgroup>` shows as a bold label in dropdown
- `<select multiple>` shows as a scrollable list box
- `size` attribute controls visible rows

**Implementation:**
- Paint select button: text + dropdown arrow (▼)
- On click: create floating dropdown panel (like context menu)
- Position dropdown below select element (or above if near bottom)
- Render options in dropdown with hover highlight
- On select: update selected option, fire `change` event

---

### 16.5 — Shadow DOM (Basic)
**Files:** `dom.cpp`, `style.cpp`, `layout.cpp`
**What:**
- `element.attachShadow({ mode: 'open' })` — create shadow root
- Shadow root contains its own DOM subtree
- Styles inside shadow DOM don't leak out
- Styles outside don't affect shadow DOM (except inherited properties)
- `<slot>` elements project light DOM children into shadow DOM

**Implementation (simplified):**
- Shadow root stored as special child of host element
- During style resolution: switch to shadow stylesheet when entering shadow tree
- `<slot>`: replace with projected children from light DOM during layout
- Limit: no named slots, no slot change events

---

### 16.6 — Custom Elements
**Files:** `quickjs_adapter.cpp`, `dom.cpp`
**What:**
- `customElements.define('my-element', class extends HTMLElement { ... })`
- `connectedCallback()` — called when element enters DOM
- `disconnectedCallback()` — called when removed
- `attributeChangedCallback()` — called when observed attribute changes
- `observedAttributes` static getter — list of attributes to watch

**Implementation:**
- Registry: map of tag name → JS constructor class
- On DOM parse/insert: check if tag matches registry
- If match: call constructor, invoke connectedCallback
- On attribute change: invoke attributeChangedCallback
- Upgrade: existing elements with matching tag get upgraded when defined

---

## PHASE 17 — Advanced Web APIs (Incremental Rebuild)

**Goal:** Support Web APIs that many modern sites depend on.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~2,500 lines in `quickjs_adapter.cpp` + new files.

### 17.1 — WebSocket API
**Files:** new `websocket.cpp`, `quickjs_adapter.cpp`
**What:**
- `new WebSocket(url)` — establish WebSocket connection
- Events: `onopen`, `onmessage`, `onclose`, `onerror`
- `ws.send(data)` — send text or binary data
- `ws.close()` — close connection

**Implementation:**
- Use WinINet `InternetOpenUrl()` with upgrade request, or use WinHTTP `WinHttpWebSocketComplete()`
- Background thread for receive loop
- Message queue with WM_USER notification to main thread
- Binary: ArrayBuffer support in QuickJS

---

### 17.2 — `history.pushState` / `popState`
**Files:** `quickjs_adapter.cpp`, `main.cpp`
**What:**
- `history.pushState(state, title, url)` — add history entry without navigation
- `history.replaceState(state, title, url)` — replace current entry
- `window.onpopstate` event — fired on back/forward navigation
- `history.back()`, `history.forward()`, `history.go(n)`
- Update address bar URL without page reload

---

### 17.3 — Clipboard API
**Files:** `quickjs_adapter.cpp`, `main.cpp`
**What:**
- `navigator.clipboard.writeText(text)` — copy text to clipboard
- `navigator.clipboard.readText()` — read text from clipboard (requires user gesture)
- `document.execCommand('copy')` — legacy clipboard access
- `copy`/`paste`/`cut` events on elements

---

### 17.4 — `URL` / `URLSearchParams` API
**Files:** `quickjs_adapter.cpp`
**What:**
- `new URL(url, base)` — parse URL
- Properties: `href`, `origin`, `protocol`, `host`, `hostname`, `port`, `pathname`, `search`, `hash`
- `url.searchParams` — `URLSearchParams` object
- `URLSearchParams.get(name)`, `.set(name, value)`, `.append()`, `.delete()`, `.has()`, `.toString()`
- `for (const [key, value] of params)` — iterable

---

### 17.5 — `FormData` API
**Files:** `quickjs_adapter.cpp`
**What:**
- `new FormData(formElement)` — collect form data
- `new FormData()` — empty, build manually
- `.append(name, value)`, `.set()`, `.get()`, `.getAll()`, `.delete()`, `.has()`
- Used with `fetch()` for form submission
- `multipart/form-data` encoding for file uploads

---

### 17.6 — Intersection Observer
**Files:** `quickjs_adapter.cpp`, `main.cpp`
**What:**
- `new IntersectionObserver(callback, { threshold: 0.5, root: null })`
- `observer.observe(element)` — watch element visibility
- Callback fired when element crosses visibility threshold
- Uses: lazy loading images, infinite scroll, analytics
- Check intersection on scroll events and layout changes

---

### 17.7 — Mutation Observer
**Files:** `quickjs_adapter.cpp`
**What:**
- `new MutationObserver(callback)`
- `observer.observe(target, { childList: true, attributes: true, subtree: true })`
- Callback fired when observed DOM changes occur
- Queue mutations, deliver in microtask (after current JS execution)
- MutationRecord: type, target, addedNodes, removedNodes, attributeName, oldValue

---

### 17.8 — Resize Observer
**Files:** `quickjs_adapter.cpp`, `layout.cpp`
**What:**
- `new ResizeObserver(callback)`
- `observer.observe(element)`
- Callback fired when element's content rect size changes
- Track observed elements, compare dimensions after each layout pass
- Deliver notifications asynchronously

---

## PHASE 18 — Multimedia & File APIs (Full Rebuild Required)

**Goal:** Video playback, audio, file handling.
**Build:** `build.ps1` (full rebuild) — needs media libraries.
**Estimated scope:** ~3,000 lines + library integration.

### 18.1 — `<video>` Playback
**New dependency:** FFmpeg (libavcodec, libavformat, libswscale) or Windows Media Foundation
**Files:** new `media.cpp`, `layout.cpp`, `renderer.cpp`, `quickjs_adapter.cpp`
**What:**

**Option A: Windows Media Foundation (simpler, Windows-only):**
- Use `IMFSourceReader` to decode video frames
- Supports: H.264, H.265, VP9, MP4, WebM (via OS codecs)
- Render decoded frames to pixel buffer

**Option B: FFmpeg (portable, more formats):**
- Integrate libavcodec for decoding
- Significant library (~5MB compiled)

**HTML API:**
- `<video src="video.mp4" controls width="640" height="480">`
- `video.play()`, `video.pause()`, `video.currentTime`
- `video.volume`, `video.muted`
- Controls overlay: play/pause button, seek bar, time display, volume, fullscreen
- `autoplay`, `loop`, `muted` attributes
- `poster` attribute (already placeholder in Phase 9)

---

### 18.2 — `<audio>` Element
**Files:** `media.cpp`, `quickjs_adapter.cpp`
**What:**
- Same decoding pipeline as video but audio-only
- `<audio src="audio.mp3" controls>`
- Windows: `waveOutOpen()` / `IAudioClient` for audio output
- Controls: play/pause, seek, volume, time display
- Formats: MP3, AAC, OGG, WAV

---

### 18.3 — Web Audio API (Basic)
**Files:** new `web_audio.cpp`, `quickjs_adapter.cpp`
**What:**
- `new AudioContext()`
- `audioContext.createBufferSource()` — play audio buffer
- `audioContext.createOscillator()` — generate tones
- `audioContext.createGain()` — volume control
- `node.connect(destination)` — audio graph routing
- `audioContext.destination` — speakers output
- Simplified: no filters, no convolution, no spatial audio

---

### 18.4 — File API / FileReader
**Files:** `quickjs_adapter.cpp`
**What:**
- `<input type="file">` — file picker dialog
- `FileReader.readAsText(file)` — read as text
- `FileReader.readAsDataURL(file)` — read as base64 data URL
- `FileReader.readAsArrayBuffer(file)` — read as binary
- Events: `onload`, `onerror`, `onprogress`
- `URL.createObjectURL(blob)` — create blob URL for display

**Implementation:**
- `GetOpenFileNameA()` for file picker dialog
- Read file contents into JS ArrayBuffer/string
- Blob URL: store in memory map, return `blob:` URL

---

### 18.5 — Drag & Drop API
**Files:** `main.cpp`, `quickjs_adapter.cpp`
**What:**
- `draggable="true"` attribute on elements
- Events: `dragstart`, `drag`, `dragenter`, `dragover`, `dragleave`, `drop`, `dragend`
- `event.dataTransfer` — data being dragged
- `dataTransfer.setData(type, data)` / `getData(type)`
- OS integration: `WM_DROPFILES` for file drops from explorer

---

## PHASE 19 — DevTools & Developer Experience (Incremental Rebuild)

**Goal:** Built-in developer tools for debugging.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~3,000 lines across new files.

### 19.1 — DOM Inspector
**Files:** new `devtools.cpp`, `renderer.cpp`, `main.cpp`
**What:**
- `F12` or `Ctrl+Shift+I` opens DevTools panel (split pane at bottom or right)
- Element tree view: expandable/collapsible DOM tree
- Click element in page → highlight in tree (and vice versa)
- Hover element in tree → blue overlay on page showing element box
- Show element attributes, text content
- Edit attributes/text inline (modify DOM, re-layout)

---

### 19.2 — Style Inspector
**Files:** `devtools.cpp`
**What:**
- Select element → show computed styles panel
- Show which CSS rules apply (with source file/line)
- Show specificity for each rule
- Crossed-out styles (overridden by higher specificity)
- Box model diagram (margin/padding/border/content with pixel values)
- Edit property values live → instant re-layout/repaint

---

### 19.3 — Console Panel
**Files:** `devtools.cpp`, `quickjs_adapter.cpp`
**What:**
- Show `console.log/error/warn` output with timestamps
- Input line at bottom for JS evaluation
- Execute JS in page context
- Object inspection: expandable object/array display
- Error stack traces with clickable file:line references

---

### 19.4 — Network Panel
**Files:** `devtools.cpp`, `net.cpp`
**What:**
- List all network requests with: URL, method, status, size, time, type
- Click request → show headers (request + response), body, timing
- Filter by type: XHR, CSS, JS, Images, Fonts
- Track requests in real-time during page load
- Show waterfall timeline

---

## PHASE 20 — Browser Polish & Platform Features (Incremental Rebuild)

**Goal:** Final polish to make the browser feel complete.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~2,000 lines across multiple files.

### 20.1 — Full-Screen Mode (F11)
**What:**
- `F11` toggles fullscreen (remove title bar, taskbar)
- `SetWindowLong()` to remove `WS_OVERLAPPEDWINDOW`
- `SetWindowPos()` to fill screen
- Restore on `F11` or `Escape`
- JS: `element.requestFullscreen()` / `document.exitFullscreen()`

### 20.2 — Multiple Windows
**What:**
- `Ctrl+N` opens new window
- Each window is independent (own layout, own tab set)
- Share image cache and cookie store across windows
- `window.open()` JS API creates new window

### 20.3 — Tab Pinning & Grouping
**What:**
- Right-click tab → "Pin Tab" → tab shrinks to icon-only width, locked to left
- Tab groups: color-coded groups with collapsible label
- Drag tabs to reorder

### 20.4 — Password Manager (Basic)
**What:**
- Detect login forms (input[type=password])
- On form submit: prompt "Save password?"
- Store credentials encrypted in `~/.scratch_browser/passwords.json`
- Auto-fill on recognized login pages
- Encryption: Windows DPAPI (`CryptProtectData`)

### 20.5 — Autofill
**What:**
- Detect common form fields by name/id/autocomplete attribute
- Store: name, email, phone, address
- Suggest saved values in dropdown below input
- `autocomplete` attribute hints: `name`, `email`, `tel`, `street-address`, etc.

### 20.6 — Spell Checker
**What:**
- Use Windows spell check API (`ISpellChecker` from Windows 8+)
- Check text in `<input>` and `<textarea>` elements
- Red squiggly underline for misspelled words
- Right-click for suggestions

### 20.7 — Print / Print Preview
**What:**
- `Ctrl+P` opens print dialog
- Generate print layout: paginate content, apply `@media print` styles
- Remove backgrounds by default, use black text
- Page headers/footers: URL, date, page number
- Windows: `StartDoc()`, `StartPage()`, render to printer DC

### 20.8 — PDF Viewer
**What:**
- Detect PDF content-type or .pdf extension
- Option A: Render PDF pages as images using Windows PDF API (`PdfDocument` from Windows 8.1+)
- Option B: Integrate MuPDF library for cross-platform rendering
- Display pages vertically with scroll
- Basic controls: zoom, page navigation, text search

### 20.9 — Incognito Mode
**What:**
- `Ctrl+Shift+N` opens incognito window
- Purple/dark theme to visually distinguish
- No cookies/history/cache persisted
- On close: all session data discarded
- Separate cookie store from normal windows

### 20.10 — Accessibility (Basic)
**What:**
- ARIA role attributes: map to Windows accessibility tree
- `IAccessible` interface implementation
- Screen reader support: expose element text, role, state
- Focus indicators: visible outline on focused elements
- Skip-to-content landmark navigation
- High-contrast mode detection: `@media (forced-colors: active)` already handled

---

## PHASE 21 — Performance & Optimization (Incremental Rebuild)

**Goal:** Make the browser fast enough for daily use.
**Build:** `rebuild.ps1` only.
**Estimated scope:** ~2,000 lines across multiple files.

### 21.1 — HTTP Connection Pooling & Keep-Alive
**What:**
- Reuse TCP connections for same-origin requests
- `Connection: keep-alive` header
- WinINet handles this automatically if using `InternetOpen()` with `INTERNET_FLAG_KEEP_CONNECTION`
- Connection pool: max 6 connections per origin

### 21.2 — HTTP/2 Support
**What:**
- WinHTTP (instead of WinINet) supports HTTP/2 on Windows 10+
- Multiplexed requests over single connection
- Header compression (HPACK)
- Server push (receive resources before requesting)
- Migration: switch from `InternetOpenUrlA` to `WinHttpOpen` + `WinHttpConnect` + `WinHttpOpenRequest`

### 21.3 — Gzip/Brotli Decompression
**What:**
- Send `Accept-Encoding: gzip, deflate, br` header
- Decompress response body:
  - gzip/deflate: `zlib` (or Windows built-in `RtlDecompressBuffer`)
  - brotli: integrate brotli library (~100KB)
- Most sites serve compressed content — significant bandwidth reduction

### 21.4 — DNS Prefetching & Preconnect
**What:**
- `<link rel="dns-prefetch" href="//cdn.example.com">` — resolve DNS early
- `<link rel="preconnect" href="https://cdn.example.com">` — establish connection early
- `<link rel="preload" href="style.css" as="style">` — fetch resource early
- Background thread for DNS resolution and connection establishment

### 21.5 — Speculative HTML Parsing
**What:**
- While waiting for CSS/JS to load, continue parsing HTML ahead
- Discover resource URLs early (images, scripts, stylesheets)
- Start fetching resources before they're needed
- "Preload scanner" pattern used by Chrome/Firefox

### 21.6 — Off-Main-Thread Scrolling
**What:**
- Move scroll handling to a separate thread
- Repaint content at scroll position without waiting for main thread layout
- Requires: scroll-invariant display list (cache painted content in tiles)
- Tile-based rendering: divide page into fixed-size tiles, paint each independently
- On scroll: recompose tiles at new offset, only paint newly visible tiles

### 21.7 — GPU Compositing (Optional)
**What:**
- Use Direct2D/Direct3D for compositing layers
- Hardware-accelerated: opacity, transforms, filters
- Layer tree: elements with `will-change`, `transform`, `opacity` get own layers
- Composite layers on GPU — much faster than software blending
- Significant architectural change — optional for this project

---

## Phase Summary (Complete)

| Phase | Focus | Build Type | Key Features | Est. LOC |
|-------|-------|------------|-------------|----------|
| 1 | Core CSS fixes | Incremental | text-transform, list-style, background-size, visibility, pseudo-classes | ~2,500 |
| 2 | Table + Float layout | Incremental | Full table algorithm, float + clear | ~1,800 |
| 3 | Browser features | Incremental | Text selection, find-in-page, zoom, context menu, view source | ~1,500 |
| 4 | Networking + Forms | Incremental | HTTP POST, cookies, form submission | ~800 |
| 5 | Animations | Incremental | CSS transitions, @keyframes, rAF | ~1,200 |
| 6 | Image + Rendering | **Full rebuild** | WebP, DirectWrite text, animated GIF | ~600 |
| 7 | JS APIs | Incremental | fetch(), classList, events, getComputedStyle | ~1,000 |
| 8 | Advanced Layout | Incremental | Grid, multi-column, flex order, intrinsic sizing | ~1,500 |
| 9 | Embedded Content | Incremental | iframe, canvas 2D, video poster | ~2,000 |
| 10 | Performance + Polish | Incremental | Incremental layout, lazy loading, bookmarks, history | ~1,000 |
| 11 | Web Fonts + Selectors | **Full rebuild** | @font-face, WOFF2, @import, :is/:has/:where, CSS nesting | ~1,800 |
| 12 | Advanced CSS Visual | Incremental | Multiple backgrounds, border-image, clip-path, blend modes, backdrop-filter, mask, aspect-ratio, scroll-snap | ~1,200 |
| 13 | Container Queries | Incremental | @container, @supports evaluation, contain, content-visibility | ~800 |
| 14 | Writing Modes + i18n | Incremental | writing-mode, direction, BiDi, IME input, logical properties | ~1,500 |
| 15 | Security + Standards | Incremental | CORS, CSP, HTTPS UI, meta http-equiv, viewport meta | ~1,200 |
| 16 | Web Components + HTML5 | Incremental | details/summary, dialog, progress/meter, select dropdown, Shadow DOM, custom elements | ~2,000 |
| 17 | Advanced Web APIs | Incremental | WebSocket, pushState, clipboard, URL, FormData, IntersectionObserver, MutationObserver, ResizeObserver | ~2,500 |
| 18 | Multimedia + Files | **Full rebuild** | Video/audio playback, Web Audio, File API, drag & drop | ~3,000 |
| 19 | DevTools | Incremental | DOM inspector, style inspector, console, network panel | ~3,000 |
| 20 | Browser Polish | Incremental | Fullscreen, multi-window, tab pinning, passwords, autofill, spell check, print, PDF, incognito, accessibility | ~2,000 |
| 21 | Performance | Incremental | HTTP/2, gzip/brotli, DNS prefetch, speculative parsing, off-thread scroll, GPU compositing | ~2,000 |

**Total estimated: ~33,900 lines of new code across 21 phases.**

Full rebuild required in: **Phase 6** (WebP/DirectWrite), **Phase 11** (WOFF2/web fonts), **Phase 18** (FFmpeg/media).
All other phases: incremental rebuild only.

Phases 1-2 should be done first (foundation). Phases 3-21 can be reordered based on priority.
