@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if not exist "%SCRIPT_DIR%build_dxvk_all_ninja.ps1" (
  echo [build] ERROR: build_dxvk_all_ninja.ps1 was not found next to build.bat.
  echo [build] Put build.bat, build_dxvk_all_ninja.ps1, build_common.ps1, meson.build, and meson_options.txt in the repo root.
  endlocal & exit /b 1
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build_dxvk_all_ninja.ps1" %*
set "EXITCODE=%ERRORLEVEL%"

endlocal & exit /b %EXITCODE%
