@echo off
rem Regenerates the real Mach-O fixtures with LLVM (clang + ld64.lld + llvm-lipo) from Windows and
rem refreshes the golden llvm-objdump snapshots. Run through the LLVM environment helper:
rem     tools\llvm-env.cmd tools\gen-real-fixtures.cmd
rem Sources, the hand-written libSystem stub (sysroot\usr\lib\libSystem.tbd) and the outputs live in
rem tests\fixtures\real. No Apple SDK or Apple code is involved.
rem Linking runs from the output directory with relative names so no absolute (user) path ends up
rem in the binaries (ad-hoc signature identifiers are derived from the output path).
setlocal
set "R=%~dp0..\tests\fixtures\real"
for %%i in ("%R%") do set "R=%%~fi"
if exist "%R%\obj" rmdir /s /q "%R%\obj"
if exist "%R%\bin" rmdir /s /q "%R%\bin"
mkdir "%R%\obj" "%R%\bin" "%R%\golden" 2>nul

set "CFLAGS_X=--target=x86_64-apple-macos10.15 -O1 -ffreestanding -nostdinc -fno-stack-protector -fno-ident -c"
set "CFLAGS_A=--target=arm64-apple-macos11 -O1 -ffreestanding -nostdinc -fno-stack-protector -fno-ident -c"

pushd "%R%\src"
for %%s in (hello foo app weak) do (
  clang %CFLAGS_X% %%s.c -o ..\obj\%%s_x86_64.o || exit /b 1
)
clang %CFLAGS_A% hello.c -o ..\obj\hello_arm64.o || exit /b 1
popd

pushd "%R%\bin"
set "LD_X=ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot ..\sysroot"
set "LD_A=ld64.lld -arch arm64 -platform_version macos 11.0.0 12.0.0 -syslibroot ..\sysroot"

rem --- executables
%LD_X% -lSystem -o hello_x86_64 ..\obj\hello_x86_64.o || exit /b 1
%LD_A% -lSystem -o hello_arm64 ..\obj\hello_arm64.o || exit /b 1
llvm-lipo -create hello_x86_64 hello_arm64 -output hello_universal || exit /b 1
%LD_X% -lSystem -adhoc_codesign -o hello_signed_x86_64 ..\obj\hello_x86_64.o || exit /b 1
%LD_X% -lSystem -fixup_chains -o hello_chained_x86_64 ..\obj\hello_x86_64.o || exit /b 1

rem --- dylibs and an app that depends on them (load + weak + rpath)
%LD_X% -dylib -install_name @rpath/libfoo.dylib -lSystem -o libfoo.dylib ..\obj\foo_x86_64.o || exit /b 1
%LD_X% -dylib -install_name @rpath/libweak.dylib -lSystem -o libweak.dylib ..\obj\weak_x86_64.o || exit /b 1
%LD_X% -lSystem libfoo.dylib -weak_library libweak.dylib -rpath @executable_path/../Frameworks -rpath @loader_path/lib -o app_x86_64 ..\obj\app_x86_64.o || exit /b 1

rem --- relocatable object (MH_OBJECT)
copy /y ..\obj\hello_x86_64.o hello_x86_64.o >nul

rem --- golden snapshots from the independent tool
for %%f in (hello_x86_64 hello_arm64 hello_signed_x86_64 hello_chained_x86_64 libfoo.dylib libweak.dylib app_x86_64 hello_x86_64.o) do (
  llvm-objdump --macho --private-headers %%f > ..\golden\%%f.private-headers.txt || exit /b 1
  llvm-objdump --macho --syms %%f > ..\golden\%%f.syms.txt || exit /b 1
)
llvm-objdump --macho --universal-headers hello_universal > ..\golden\hello_universal.universal-headers.txt || exit /b 1
llvm-objdump --version | findstr /b /c:"  LLVM version" > ..\golden\TOOL-VERSION.txt
clang --version | findstr /b /c:"clang version" >> ..\golden\TOOL-VERSION.txt
ld64.lld --version >> ..\golden\TOOL-VERSION.txt
popd
rmdir /s /q "%R%\obj"
echo done
exit /b 0
