# Browser Engine

A C++ web browser engine built from scratch. It features its own layout engine, a DOM parser using Lexbor, JavaScript support via QuickJS, and SVG/vector graphics using LunaSVG and PlutoVG.

## Features
- HTML5 Parsing
- CSS Layout Engine with inline, block, and basic tables/floats support
- JavaScript Execution
- Network loading (HTTP/HTTPS)
- Graphics rendering with ClearType support

## Build
Use the provided `build.ps1` to perform a full build or `rebuild.ps1` for an incremental build.

```ps1
# Full rebuild
.\build.ps1

# Incremental rebuild
.\rebuild.ps1
```
