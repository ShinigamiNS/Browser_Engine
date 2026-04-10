@echo off
set "PATH=C:\MinGW\bin;%PATH%"
echo Building Scratch Browser Engine...
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1"
pause
