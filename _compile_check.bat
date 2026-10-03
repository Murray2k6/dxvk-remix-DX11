@echo off
rem Compiles one bridge server object (main.cpp) as a quick compile check.
rem Paths are relative to this script; Visual Studio 18 (2026) is the toolchain the builds use.
cd /d "%~dp0bridge_dx11_work\_Comp64Release"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "NINJA=%~dp0.build_deps\python\bin\ninja.exe"
"%NINJA%" "src/server/NvRemixBridge.exe.p/main.cpp.obj"
