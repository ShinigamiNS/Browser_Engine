@echo off
:: setup_lexbor.bat — Run this ONCE before building
set "PATH=C:\MinGW\bin;%PATH%"

echo ============================================
echo  Lexbor Setup — HTML5 Parser for Browser
echo ============================================
echo.

:: Check git is available
where git >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: git not found. Install Git for Windows first:
    echo   https://git-scm.com/download/win
    pause
    exit /b 1
)

:: Clone Lexbor
if exist third_party\lexbor (
    echo Lexbor already cloned. Skipping.
) else (
    echo Cloning Lexbor...
    mkdir third_party 2>nul
    git clone --depth=1 https://github.com/lexbor/lexbor.git third_party\lexbor
    if %ERRORLEVEL% neq 0 (
        echo ERROR: git clone failed. Check your internet connection.
        pause
        exit /b 1
    )
    echo Lexbor cloned successfully.
)

echo.
echo Verifying key source files...
if exist third_party\lexbor\source\lexbor\html\parser.c (
    echo   OK: html/parser.c found
) else (
    echo   ERROR: Unexpected directory structure.
    echo   Expected: third_party\lexbor\source\lexbor\html\parser.c
    pause
    exit /b 1
)

if exist third_party\lexbor\source\lexbor\core\mem.c (
    echo   OK: core/mem.c found
) else (
    echo   ERROR: core files missing.
    pause
    exit /b 1
)

echo.
echo ============================================
echo  Setup complete! Now run: build.bat
echo ============================================
pause
