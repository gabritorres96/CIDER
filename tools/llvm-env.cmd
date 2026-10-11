@echo off
rem Runs a command with the MSVC environment (headers/libs) and the portable LLVM from
rem <repo>\.toolchain on PATH. Usage: tools\llvm-env.cmd cmake --build build-asan
rem Override the vcvars location with ORVYN_VCVARS if Visual Studio is installed elsewhere.
if not defined ORVYN_VCVARS set "ORVYN_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%ORVYN_VCVARS%" >nul
if errorlevel 1 (echo vcvars64.bat failed: %ORVYN_VCVARS% & exit /b 1)
for /d %%d in ("%~dp0..\.toolchain\clang+llvm-*") do set "LLVM_ROOT=%%~fd"
if not defined LLVM_ROOT (echo portable LLVM not found in .toolchain & exit /b 1)
set "PATH=%LLVM_ROOT%\bin;%PATH%"
%*
