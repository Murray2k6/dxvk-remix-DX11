@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_dx11_all.ps1" %*
exit /b %ERRORLEVEL%
