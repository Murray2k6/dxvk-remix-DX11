@echo off
setlocal
set ROOT=%~dp0
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%Apply-DX11VisualRuntimeFixes-V58.ps1"
if errorlevel 1 exit /b %ERRORLEVEL%
call "%ROOT%RUN_BUILD_DX11_DUAL_OUTPUT_V58.bat" %*
exit /b %ERRORLEVEL%
