@echo off
rem Builds the ORVYN 0.2 execution fixtures: real Mach-O x86_64 executables (clang + ld64.lld, linked
rem against the hand-written libSystem stub in tests\fixtures\exec\sysroot) plus llvm-objdump snapshots.
rem     tools\llvm-env.cmd tools\gen-exec-fixtures.cmd
rem Every executable uses the LC_MAIN protocol: main(argc, argv, envp, apple), return value -> exit().
rem Relative paths only, so no user path ends up in the binaries.
setlocal
set "R=%~dp0..\tests\fixtures\exec"
for %%i in ("%R%") do set "R=%%~fi"
if exist "%R%\obj" rmdir /s /q "%R%\obj"
if exist "%R%\bin" rmdir /s /q "%R%\bin"
mkdir "%R%\obj" "%R%\bin" "%R%\golden" 2>nul

set "NAMES=ret42 ret7 ret99 math stack calls globals bss libcall hello args missing exitcall crash spin malloc fmath recurse hog pid printf_int printf_double printf_str printf_mixed snprintf stdio_out files_read files_write time_clock abi_ret unsupported ids open_path qsort_ints qsort_records qsort_nested atexit_order atexit_exit atexit_skipped qsort_exit qsort_bad streams_write streams_read streams_limit thread_basic thread_mutex thread_race thread_tls thread_sync thread_exit"
set "CFLAGS=--target=x86_64-apple-macos10.15 -O1 -ffreestanding -nostdinc -fno-builtin -fno-stack-protector -fno-ident -c"

pushd "%R%\src"
for %%s in (%NAMES%) do (
  clang %CFLAGS% %%s.c -o ..\obj\%%s.o || exit /b 1
)
popd

pushd "%R%\bin"
for %%s in (%NAMES%) do (
  ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot ..\sysroot -lSystem -o %%s ..\obj\%%s.o || exit /b 1
  llvm-objdump --macho --private-headers %%s > ..\golden\%%s.private-headers.txt || exit /b 1
  llvm-objdump --macho --syms %%s > ..\golden\%%s.syms.txt || exit /b 1
  llvm-objdump --macho -d %%s > ..\golden\%%s.disasm.txt || exit /b 1
  llvm-objdump --macho --rebase --bind --lazy-bind %%s > ..\golden\%%s.fixups.txt || exit /b 1
)
llvm-objdump --version | findstr /b /c:"  LLVM version" > ..\golden\TOOL-VERSION.txt
clang --version | findstr /b /c:"clang version" >> ..\golden\TOOL-VERSION.txt
ld64.lld --version >> ..\golden\TOOL-VERSION.txt
popd
rmdir /s /q "%R%\obj"
echo done
exit /b 0
