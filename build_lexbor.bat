@echo off
setlocal enabledelayedexpansion

set SRC=third_party\lexbor\source
set OUTDIR=third_party\lexbor\lexbor_objs
set LIB=third_party\lexbor\liblexbor_static.a

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set ERRCOUNT=0
set COUNT=0

for /r "%SRC%" %%F in (*.c) do (
    set /a COUNT+=1
    set "RPATH=%%~dpnF"
    set "RPATH=!RPATH:%CD%\%SRC%\=!"
    set "RPATH=!RPATH:\=_!"
    set "OBJNAME=!RPATH!.o"
    gcc -O2 -w -I%SRC% -c "%%F" -o "%OUTDIR%\!OBJNAME!" 2>nul
    if errorlevel 1 (
        set /a ERRCOUNT+=1
        echo FAILED: %%F
    )
)

echo Compiled %COUNT% files, %ERRCOUNT% errors

rem Collect all .o files and archive
set OBJLIST=
for %%O in ("%OUTDIR%\*.o") do (
    set "OBJLIST=!OBJLIST! %%O"
)

ar rcs "%LIB%" %OBJLIST%
if errorlevel 1 (
    echo ar FAILED
) else (
    echo Library built: %LIB%
)
