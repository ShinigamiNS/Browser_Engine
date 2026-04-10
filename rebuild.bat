@echo off
set "PATH=C:\MinGW\bin;%PATH%"
echo Rebuilding browser sources (skipping third-party)...
powershell -ExecutionPolicy Bypass -File "%~dp0rebuild.ps1"
pause
