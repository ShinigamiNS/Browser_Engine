# build_lexbor.ps1 — compile lexbor C sources into a static library without cmake
param()

$base = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $base "third_party\lexbor\source"
$odir = Join-Path $base "third_party\lexbor\lexbor_objs"
$lib  = Join-Path $base "third_party\lexbor\liblexbor_static.a"

New-Item -ItemType Directory -Force -Path $odir | Out-Null

$files = Get-ChildItem $src -Recurse -Filter "*.c"
$count = 0; $errs = 0

foreach ($f in $files) {
    # Make a flat object name: replace dir separators with __
    $rel = $f.FullName.Substring($src.Length + 1).Replace("\","__").Replace(".c",".o")
    $obj = Join-Path $odir $rel

    # Ensure subdirectory for obj exists (not needed since flat, but safety)
    $objDir = Split-Path $obj
    if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Force -Path $objDir | Out-Null }

    $gcc_out = & gcc -O2 -w "-DLXB_API=" "-DLEXBOR_API=" "-DLEXBOR_STATIC" ("-I" + $src) -c $f.FullName -o $obj 2>&1
    if ($LASTEXITCODE -ne 0) {
        $errs++
        Write-Host ("FAIL: " + $f.Name + " -- " + ($gcc_out -join " "))
    }
    $count++
    if ($count % 30 -eq 0) { Write-Host "  $count / $($files.Count) compiled..." }
}

Write-Host "Compiled $count files, $errs errors"

# Remove old lib
if (Test-Path $lib) { Remove-Item $lib }

# Archive incrementally (avoid command-line length limits)
$objs = Get-ChildItem $odir -Filter "*.o" | Select-Object -ExpandProperty FullName
$batch = 50
for ($i = 0; $i -lt $objs.Count; $i += $batch) {
    $chunk = $objs[$i..([Math]::Min($i + $batch - 1, $objs.Count - 1))]
    $ar_out = & ar rcs $lib $chunk 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host ("ar error: " + ($ar_out -join " ")) }
}

if (Test-Path $lib) {
    $sz = (Get-Item $lib).Length
    Write-Host "SUCCESS: $lib ($sz bytes)"
} else {
    Write-Host "FAILED: library not created"
}
