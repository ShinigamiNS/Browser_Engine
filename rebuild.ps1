$env:PATH = "C:\MinGW\bin;" + $env:PATH

$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path $root "src"
$objDir = Join-Path $root "obj"
$output = Join-Path $root "BrowserEngine.exe"

$lexborSrc = Join-Path $root "third_party\lexbor\source"
$qjsSrc    = Join-Path $root "third_party\quickjs"

# ── Guard: third-party obj files must already exist ───────────────────────
if (-not (Test-Path $objDir)) {
    Write-Host "ERROR: obj\ not found. Run build.bat first to compile third-party libs."
    exit 1
}

$existingObjs = Get-ChildItem -Path $objDir -Filter "*.o" -File
if ($existingObjs.Count -eq 0) {
    Write-Host "ERROR: obj\ is empty. Run build.bat first to compile third-party libs."
    exit 1
}

# ── Step 1: Recompile browser C++ sources only ─────────────────────────────
Write-Host "Compiling browser sources..."

$browserSrcs = @(
    "src\main.cpp", "src\image_cache.cpp", "src\page_loader.cpp", "src\renderer.cpp",
    "src\svg_rasterizer.cpp",
    "src\lexbor_adapter.cpp", "src\quickjs_adapter.cpp",
    "src\browser_ui.cpp", "src\browser_tabs.cpp", "src\browser_address.cpp",
    "src\dom.cpp", "src\html_parser.cpp",
    "src\css_parser.cpp", "src\style.cpp", "src\selector.cpp", "src\ua_styles.cpp",
    "src\ua_stylesheet.cpp",
    "src\layout.cpp", "src\layout_inline.cpp", "src\css_values.cpp",
    "src\paint.cpp", "src\color_utils.cpp",
    "src\net.cpp", "src\js.cpp", "src\font_metrics.cpp"
) | ForEach-Object { Join-Path $root $_ }

$lunasvgInc = Join-Path $root "third_party\lunasvg\include"
$lunasvgSrc = Join-Path $root "third_party\lunasvg\source"
$plutovgSrc = Join-Path $root "third_party\lunasvg\3rdparty\plutovg"

$browserObjs = @()
foreach ($src in $browserSrcs) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj  = Join-Path $objDir "${name}.o"
    $browserObjs += $obj
    Write-Host "  Compiling $name..."
    & g++ -c -w -std=c++14 -O2 -DLEXBOR_STATIC -DLUNASVG_BUILD_STATIC `
        "-I$lexborSrc" "-I$qjsSrc" "-I$srcDir" `
        "-I$lunasvgInc" "-I$lunasvgSrc" "-I$plutovgSrc" $src -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: $name"; exit 1 }
}
Write-Host "  Done."

# ── Step 2: Link (browser objs + all pre-built third-party objs) ───────────
Write-Host "Linking..."

$thirdPartyObjs = Get-ChildItem -Path $objDir -Filter "*.o" -File |
    Where-Object { $browserObjs -notcontains $_.FullName } |
    Select-Object -ExpandProperty FullName

& g++ ($browserObjs + $thirdPartyObjs) -o $output `
    -lgdi32 -luser32 -lwininet -lmsimg32 -lgdiplus -lkernel32 -lm

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "Build successful! Run BrowserEngine.exe"
} else {
    Write-Host "Link failed."; exit 1
}
