// quickjs_adapter.h
// QuickJS integration - full ES2020 JavaScript engine
//
// Setup (one time):
//   git clone https://github.com/bellard/quickjs.git third_party/quickjs
//   (no build step needed - we compile the .c files directly)
//
// Add to build:
//   src/quickjs_adapter.cpp
//   third_party/quickjs/quickjs.c
//   third_party/quickjs/libregexp.c
//   third_party/quickjs/libunicode.c
//   third_party/quickjs/cutils.c
//   third_party/quickjs/libbf.c   (if exists, for BigInt)
//   Compile with: -DCONFIG_BIGNUM (optional, for full spec)

#pragma once
#include "dom.h"
#include <string>
#include <memory>
#include <functional>

// Initialize QuickJS and set up DOM bindings
// Call once per page load, pass the DOM root
struct QJSEngine;
QJSEngine* qjs_create(std::shared_ptr<Node> dom_root);
void        qjs_destroy(QJSEngine* engine);

// Execute a script string. Returns error message or "" on success.
std::string qjs_eval(QJSEngine* engine, const std::string& source,
                     const std::string& filename = "<script>");

// Run all <script> tags in the DOM (call after page load)
void qjs_run_scripts(QJSEngine* engine, std::shared_ptr<Node> root);

// Call a named global function if it exists (e.g. "onload")
void qjs_call_global(QJSEngine* engine, const std::string& name);

// Check if DOM was mutated since last call (triggers re-layout in main.cpp)
bool qjs_dom_dirty(QJSEngine* engine);
void qjs_clear_dirty(QJSEngine* engine);

// Fire a DOM event on a specific node (e.g. "click", "input").
// clientX/Y are mouse coordinates (0 if not a mouse event).
void qjs_fire_event(QJSEngine* engine, Node* node,
                    const std::string& type,
                    int clientX = 0, int clientY = 0);

// Set viewport dimensions so window.innerWidth/innerHeight are correct.
void qjs_set_viewport(QJSEngine* engine, int width, int height);

// Set the current page URL so window.location reflects reality.
void qjs_set_page_url(QJSEngine* engine, const std::string& url);

// Register a layout-query callback so JS can call getBoundingClientRect().
// The callback receives a Node* and fills out x,y,width,height; returns false
// if the node was not found in the layout tree.
#include <functional>
struct DOMRect { float x, y, width, height; };
void qjs_set_layout_cb(QJSEngine* engine,
                       std::function<bool(Node*, DOMRect&)> cb);

// Run any pending setTimeout/setInterval callbacks (call from WM_TIMER handler).
// Returns true if at least one callback was fired.
bool qjs_run_pending_timers(QJSEngine* engine);
