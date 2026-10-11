@echo off
rem Builds the ORVYN 0.4 Objective-C fixtures: real Mach-O x86_64 programs whose classes, selectors and messages are compiled by
rem clang (Apple objc2 ABI metadata, objc_msgSend calls) and linked by ld64.lld against hand-written stubs (tests\fixtures\objc\sysroot),
rem plus llvm-objdump snapshots (headers, symbols, rebase/bind). There is no Apple SDK: the headers in tests\fixtures\objc\include declare
rem only what the fixtures use.
rem     tools\llvm-env.cmd tools\gen-objc-fixtures.cmd
setlocal enabledelayedexpansion
set "R=%~dp0..\tests\fixtures\objc"
for %%i in ("%R%") do set "R=%%~fi"
if exist "%R%\obj" rmdir /s /q "%R%\obj"
if exist "%R%\bin" rmdir /s /q "%R%\bin"
if exist "%R%\golden" rmdir /s /q "%R%\golden"
mkdir "%R%\obj" "%R%\bin" "%R%\golden"
pushd "%R%"

set "COMMON=--target=x86_64-apple-macos10.15 -x objective-c -O1 -ffreestanding -nostdinc -Iinclude -Isrc -I..\exec\src -fno-stack-protector -fno-ident -c"
set "LD=ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot sysroot -lSystem -lobjc"

rem ---- single-executable programs (manual reference counting unless noted) ----------------------------------------------------
for %%n in (objc_calc objc_inherit objc_dynamic objc_category objc_abi objc_error objc_mrc objc_memory_error) do (
  clang %COMMON% src\%%n.m -o obj\%%n.o || exit /b 1
  mkdir bin\%%n
  %LD% -o bin\%%n\%%n obj\%%n.o || exit /b 1
)

rem ---- ARC: compiled with -fobjc-arc ----------------------------------------------------------------------------------------------------
clang %COMMON% -fobjc-arc -fno-objc-exceptions src\objc_arc.m -o obj\objc_arc.o || exit /b 1
mkdir bin\objc_arc
%LD% -o bin\objc_arc\objc_arc obj\objc_arc.o || exit /b 1

rem ---- Foundation subset (ORVYN 0.4.6): ARC programs and MRC programs, linked against the hand-written Foundation.tbd -----------------
set "LDF=ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot sysroot -lSystem -lobjc -framework Foundation -framework CoreFoundation -framework CoreGraphics"
for %%n in (foundation_hello foundation_string foundation_collections dispatch_basic dispatch_error foundation_extra) do (
  clang %COMMON% -fobjc-arc -fno-objc-exceptions src\%%n.m -o obj\%%n.o || exit /b 1
  mkdir bin\%%n
  %LDF% -o bin\%%n\%%n obj\%%n.o || exit /b 1
)
for %%n in (foundation_subclass foundation_error blocks_basic blocks_error cf_basic objc_forward objc_exception) do (
  clang %COMMON% -fno-objc-exceptions -fno-exceptions src\%%n.m -o obj\%%n.o || exit /b 1
  mkdir bin\%%n
  %LDF% -o bin\%%n\%%n obj\%%n.o || exit /b 1
)

rem ---- @try/@catch: compiled WITH Objective-C exceptions; ORVYN must refuse it (no unwinder) -----------------------------------------------
clang %COMMON% -fobjc-exceptions -fexceptions src\objc_try.m -o obj\objc_try.o || exit /b 1
mkdir bin\objc_try
%LDF% -o bin\objc_try\objc_try obj\objc_try.o || exit /b 1

rem ---- slide: the base class lives in a dylib that is newer than the header the executable was compiled against --------------------
clang %COMMON% src\shape_v2.m -o obj\shape_v2.o || exit /b 1
clang %COMMON% src\slide_main.m -o obj\slide_main.o || exit /b 1
mkdir bin\slide
ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot sysroot -dylib -install_name @rpath/libshape.dylib -o bin\slide\libshape.dylib obj\shape_v2.o -lSystem -lobjc || exit /b 1
ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot sysroot -rpath @executable_path -Lbin\slide -lshape -lSystem -lobjc -o bin\slide\slide obj\slide_main.o || exit /b 1

rem ---- snapshots -----------------------------------------------------------------------------------------------------------------------
for /r bin %%f in (*) do (
  set "N=%%~nxf"
  set "D=%%~dpf"
  set "REL=!D:%R%\bin\=!"
  set "KEY=!REL:\=_!!N!"
  llvm-objdump --macho --private-headers "%%f" > "golden\!KEY!.private-headers.txt" || exit /b 1
  llvm-objdump --macho --syms "%%f" > "golden\!KEY!.syms.txt" || exit /b 1
  llvm-objdump --macho --rebase --bind --lazy-bind "%%f" > "golden\!KEY!.fixups.txt" || exit /b 1
  llvm-objdump --macho --objc-meta-data "%%f" > "golden\!KEY!.objc.txt" 2>nul
)
llvm-objdump --version | findstr /b /c:"  LLVM version" > golden\TOOL-VERSION.txt
clang --version | findstr /b /c:"clang version" >> golden\TOOL-VERSION.txt
ld64.lld --version >> golden\TOOL-VERSION.txt
rmdir /s /q obj
popd
echo done
exit /b 0
