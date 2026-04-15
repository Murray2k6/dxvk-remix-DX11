@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts-common\build_with_checks.ps1" %*
set "EXITCODE=%ERRORLEVEL%"

endlocal & exit /b %EXITCODE%