@echo off
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
set "PS1=%SCRIPT_DIR%Build-DX11-DualOutput-V50.ps1"
if not exist "%PS1%" (
  echo [build] ERROR: Build-DX11-DualOutput-V50.ps1 was not found next to build.bat.
  exit /b 1
)
pushd "%SCRIPT_DIR%" >nul || exit /b 1
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "EXITCODE=%ERRORLEVEL%"
popd >nul
if "%EXITCODE%"=="0" (
  echo [build] OK - DX11 x64 and x86 output folders are ready.
) else (
  echo [build] FAILED with exit code %EXITCODE%.
)
endlocal & exit /b %EXITCODE%
