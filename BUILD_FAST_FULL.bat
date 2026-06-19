@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR="

if exist "%SCRIPT_DIR%build.bat" set "PROJECT_DIR=%SCRIPT_DIR%"
if not defined PROJECT_DIR if exist "%CD%\build.bat" set "PROJECT_DIR=%CD%\"
if not defined PROJECT_DIR if exist "C:\Users\icetr\Downloads\dxvk-remix-DX11\build.bat" set "PROJECT_DIR=C:\Users\icetr\Downloads\dxvk-remix-DX11\"

if not defined PROJECT_DIR (
  echo [dx11-v224] ERROR: Could not find dxvk-remix-DX11 repo root.
  pause
  exit /b 2
)

cd /d "%PROJECT_DIR%"

if not exist "%CD%\build.bat" (
  echo [dx11-v224] ERROR: build.bat missing in "%CD%"
  pause
  exit /b 3
)

if "%DX11_BUILD_JOBS%"=="" set "DX11_BUILD_JOBS=%NUMBER_OF_PROCESSORS%"
if "%DX11_FULL_VERBOSE%"=="" set "DX11_FULL_VERBOSE=0"

if not exist "%CD%\_build_logs" mkdir "%CD%\_build_logs"
for /f "tokens=1-4 delims=/ " %%a in ("%date%") do set "D=%%d%%b%%c"
for /f "tokens=1-3 delims=:." %%a in ("%time%") do set "T=%%a%%b%%c"
set "T=%T: =0%"
set "DX11_FAST_LOG=%CD%\_build_logs\V224_REAL_BUILD_%D%_%T%.log"

echo [dx11-v224] project="%CD%"
echo [dx11-v224] jobs=%DX11_BUILD_JOBS% verbose=%DX11_FULL_VERBOSE%
echo [dx11-v224] log="%DX11_FAST_LOG%"
echo [dx11-v224] running real build through build.bat %*
echo.

call "%CD%\build.bat" %* > "%DX11_FAST_LOG%" 2>&1
set "RC=%ERRORLEVEL%"

type "%DX11_FAST_LOG%"

echo.
echo [dx11-v224] build exit code: %RC%
echo [dx11-v224] log: "%DX11_FAST_LOG%"

if not "%RC%"=="0" (
  echo.
  echo [dx11-v224] Build failed. Upload this log:
  echo %DX11_FAST_LOG%
  pause
  exit /b %RC%
)

echo.
echo [dx11-v224] Build completed successfully.
pause
exit /b 0
