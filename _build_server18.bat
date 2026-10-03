@echo off
rem Builds only the bridge server (NvRemixBridge.exe) in the configured x64 bridge build.
rem Paths are relative to this script; Visual Studio 18 (2026) is the toolchain the builds use.
cd /d "%~dp0bridge_dx11_work\_Comp64Release"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=%~dp0.build_deps\python\bin;%PATH%"
ninja "src/server/NvRemixBridge.exe" 2>&1
echo EXITCODE=%ERRORLEVEL%
