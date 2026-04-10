$env:PATH = "C:\MinGW\bin;" + $env:PATH

$root      = Split-Path -Parent $MyInvocation.MyCommand.Path
$lexborSrc = Join-Path $root "third_party\lexbor\source"
$qjsSrc    = Join-Path $root "third_party\quickjs"
$srcDir    = Join-Path $root "src"
$output    = Join-Path $root "BrowserEngine.exe"
$objDir    = Join-Path $root "obj"

if (-not (Test-Path $lexborSrc)) {
    Write-Host "ERROR: Lexbor not found. Run setup_lexbor.bat first."
    exit 1
}
if (-not (Test-Path $qjsSrc)) {
    Write-Host "ERROR: QuickJS not found."
    Write-Host "Run: git clone https://github.com/bellard/quickjs.git third_party\quickjs"
    exit 1
}

if (Test-Path $objDir) {
    Write-Host "Cleaning obj/..."
    Remove-Item -Recurse -Force $objDir
}
New-Item -ItemType Directory -Path $objDir | Out-Null

# ── Step 1: Compile Lexbor ────────────────────────────────────────────────
$lexborFlags = @("-O2", "-DLEXBOR_STATIC", "-I$lexborSrc")

Write-Host "Step 1: Compiling Lexbor..."
$memObj = Join-Path $objDir "lexbor_memory.o"
& gcc -c -O2 (Join-Path $root "src\lexbor_memory.c") -o $memObj
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: lexbor_memory.c"; exit 1 }

$lexborFiles = Get-ChildItem -Path $lexborSrc -Recurse -Filter "*.c" |
    Where-Object { $_.FullName -notlike "*\ports\*" } |
    Select-Object -ExpandProperty FullName

$lexborObjs = @($memObj)
$i = 0
foreach ($src in $lexborFiles) {
    $i++
    $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $hash = [Math]::Abs($src.GetHashCode()).ToString()
    $obj  = Join-Path $objDir "${name}_${hash}.o"
    $lexborObjs += $obj
    & gcc -c @lexborFlags $src -o $obj 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: $([System.IO.Path]::GetFileName($src))"; exit 1 }
    if ($i % 20 -eq 0) { Write-Host "  Lexbor: $i/$($lexborFiles.Count)..." }
}
Write-Host "  Lexbor done ($i files)."

# ── Step 2: Compile QuickJS ───────────────────────────────────────────────
Write-Host "Step 2: Compiling QuickJS..."

# Copy pthread_stub.h into QuickJS source dir so -include can find it
Copy-Item (Join-Path $root "src\pthread_stub.h") (Join-Path $qjsSrc "pthread_stub.h") -Force

# Compile ALL .c files in the QuickJS directory - different versions split
# number conversion functions (js_dtoa, js_atod, i64toa etc.) into different
# files (libbf.c, qjs-dtoa.c, qjs-numconv.c depending on version).
# Compiling everything avoids hunting for which file has what.
# Only compile the core engine files - everything else is standalone executables
# or POSIX-dependent tools we don't need in the browser
$qjsCoreFiles = @("quickjs.c","libregexp.c","libunicode.c","cutils.c","libbf.c",
                  "qjs-dtoa.c","qjs-numconv.c")
$qjsCFiles = Get-ChildItem -Path $qjsSrc -Filter "*.c" -File |
    Where-Object { $qjsCoreFiles -contains $_.Name } |
    Select-Object -ExpandProperty Name
Write-Host "  QuickJS core files: $($qjsCFiles -join ', ')"
Write-Host "  QuickJS files ($($qjsCFiles.Count)): $($qjsCFiles -join ', ')"

# Compile number conversion stubs (provides js_dtoa, js_atod, i64toa, u32toa)
$numconvSrc = Join-Path $root "src\quickjs_numconv.c"
$numconvObj = Join-Path $objDir "quickjs_numconv.o"
& gcc -c -O2 -D_WIN32 -DNDEBUG "-I$qjsSrc" $numconvSrc -o $numconvObj 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling quickjs_numconv.c"; exit 1 }
Write-Host "  quickjs_numconv.c done."

$qjsObjs = @($numconvObj)
foreach ($f in $qjsCFiles) {
    $src = Join-Path $qjsSrc $f
    $obj = Join-Path $objDir ($f -replace '\.c$', '.o')
    $qjsObjs += $obj
    & gcc -c -O2 -D_WIN32 -D__MINGW32__ -DNDEBUG `
        "-I$qjsSrc" "-I$srcDir" -include pthread_stub.h `
        $src -o $obj 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR compiling QuickJS: $f"; exit 1 }
    Write-Host "  QuickJS: $f done."
}

# ── Step 3: Compile browser C++ ───────────────────────────────────────────
Write-Host "Step 3: Compiling browser sources..."
$browserSrcs = @(
    "src\main.cpp", "src\image_cache.cpp", "src\page_loader.cpp", "src\renderer.cpp",
    "src\svg_rasterizer.cpp",
    "src\lexbor_adapter.cpp", "src\quickjs_adapter.cpp",
    "src\browser_ui.cpp", "src\browser_tabs.cpp", "src\browser_address.cpp",
    "src\dom.cpp", "src\html_parser.cpp",
    "src\css_parser.cpp", "src\style.cpp", "src\selector.cpp", "src\ua_styles.cpp",
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
    & g++ -c -w -std=c++14 -O2 -DLEXBOR_STATIC -DLUNASVG_BUILD_STATIC `
        "-I$lexborSrc" "-I$qjsSrc" "-I$srcDir" `
        "-I$lunasvgInc" "-I$lunasvgSrc" "-I$plutovgSrc" $src -o $obj
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: $name"; exit 1 }
}

# Compile lunasvg C++ sources
Write-Host "  Compiling lunasvg..."
$lunasvgCppFiles = Get-ChildItem -Path $lunasvgSrc -Filter "*.cpp" | Select-Object -ExpandProperty FullName
foreach ($lsrc in $lunasvgCppFiles) {
    $lname = [System.IO.Path]::GetFileNameWithoutExtension($lsrc)
    $lobj  = Join-Path $objDir "lunasvg_${lname}.o"
    $browserObjs += $lobj
    & g++ -c -w -std=c++14 -O2 -DLUNASVG_BUILD -DLUNASVG_BUILD_STATIC `
        "-I$lunasvgInc" "-I$lunasvgSrc" "-I$plutovgSrc" $lsrc -o $lobj
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: lunasvg $lname"; exit 1 }
}

# Compile plutovg C sources
Write-Host "  Compiling plutovg..."
$plutovgCFiles = Get-ChildItem -Path $plutovgSrc -Filter "*.c" | Select-Object -ExpandProperty FullName
foreach ($psrc in $plutovgCFiles) {
    $pname = [System.IO.Path]::GetFileNameWithoutExtension($psrc)
    $pobj  = Join-Path $objDir "plutovg_${pname}.o"
    $browserObjs += $pobj
    & gcc -c -w -O2 "-I$plutovgSrc" $psrc -o $pobj
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: plutovg $pname"; exit 1 }
}

Write-Host "  Browser done."

# ── Step 4: Link ──────────────────────────────────────────────────────────
Write-Host "Step 4: Linking..."
& g++ ($browserObjs + $qjsObjs + $lexborObjs) -o $output `
    -lgdi32 -luser32 -lwininet -lmsimg32 -lgdiplus -lkernel32 -lm

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "Build successful! Run BrowserEngine.exe"
} else {
    Write-Host "Link failed."; exit 1
}
