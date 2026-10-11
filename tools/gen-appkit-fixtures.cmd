@echo off
rem Builds the CIDER 0.5 AppKit fixtures: real Mach-O x86_64 Objective-C programs (clang + ld64.lld) that create windows through the AppKit subset.
rem The hand-written headers and the framework stubs are shared with the Objective-C fixtures (tests\fixtures\objc\include and \sysroot).
rem     tools\llvm-env.cmd tools\gen-appkit-fixtures.cmd
setlocal enabledelayedexpansion
set "R=%~dp0..\tests\fixtures\appkit"
for %%i in ("%R%") do set "R=%%~fi"
set "O=%R%\..\objc"
for %%i in ("%O%") do set "O=%%~fi"
if exist "%R%\obj" rmdir /s /q "%R%\obj"
if exist "%R%\bin" rmdir /s /q "%R%\bin"
mkdir "%R%\obj" "%R%\bin"
pushd "%R%"
set "COMMON=--target=x86_64-apple-macos10.15 -x objective-c -O1 -ffreestanding -nostdinc -I"%O%\include" -Isrc -fno-stack-protector -fno-ident -fobjc-arc -fno-objc-exceptions -fno-exceptions -c"
set "LD=ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot "%O%\sysroot" -lSystem -lobjc -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework AppKit"
for %%n in (hello_window appkit_basic cg_draw_test cg_abi_test cg_unimplemented cg_lifecycle) do (
  clang %COMMON% src\%%n.m -o obj\%%n.o || exit /b 1
  mkdir bin\%%n
  %LD% -o bin\%%n\%%n obj\%%n.o || exit /b 1
)
rmdir /s /q obj
popd
echo done
exit /b 0
