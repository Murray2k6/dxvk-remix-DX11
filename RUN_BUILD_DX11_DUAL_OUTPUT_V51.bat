@echo off
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
set "PS1=%SCRIPT_DIR%Build-DX11-DualOutput-V51.ps1"
if not exist "%PS1%" (
  echo [dx11-output-v51] ERROR: Build-DX11-DualOutput-V51.ps1 was not found next to this batch file.
  exit /b 1
)
pushd "%SCRIPT_DIR%" >nul || exit /b 1
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "EXITCODE=%ERRORLEVEL%"
popd >nul
endlocal & exit /b %EXITCODE%
