@echo off
setlocal
set ROOT=%~dp0
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%Apply-DX11VisualRuntimeFixes-V57.ps1" %*
exit /b %ERRORLEVEL%
