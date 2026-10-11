@echo off
rem Builds the ORVYN 0.3 dynamic-linking fixtures: real Mach-O x86_64 executables AND dylibs (clang + ld64.lld),
rem plus llvm-objdump snapshots (headers, exports trie, symbols, rebase/bind tables).
rem     tools\llvm-env.cmd tools\gen-dylib-fixtures.cmd
rem Relative paths only, so no user path ends up in the binaries. Every scenario lives in tests\fixtures\dylib\bin\<name>.
setlocal enabledelayedexpansion
set "R=%~dp0..\tests\fixtures\dylib"
for %%i in ("%R%") do set "R=%%~fi"
if exist "%R%\obj" rmdir /s /q "%R%\obj"
if exist "%R%\bin" rmdir /s /q "%R%\bin"
if exist "%R%\golden" rmdir /s /q "%R%\golden"
mkdir "%R%\obj" "%R%\bin" "%R%\golden" 2>nul
pushd "%R%"

set "SYS=..\exec\sysroot"
set "CFLAGS=-I..\exec\src --target=x86_64-apple-macos10.15 -O1 -ffreestanding -nostdinc -fno-builtin -fno-stack-protector -fno-ident -c"
set "LD=ld64.lld -arch x86_64 -platform_version macos 10.15.0 12.0.0 -syslibroot %SYS%"

rem ---- compile every source once --------------------------------------------------------------------
for %%f in (src\*.c) do (
  if /i not "%%~nf"=="wdef2_lib" clang %CFLAGS% %%f -o obj\%%~nf.o || exit /b 1
)

rem Initializer fixtures use -O0: at -O1 LLVM's GlobalOpt evaluates a static constructor at compile time and the
rem __mod_init_func section would disappear, which defeats the purpose of the fixture.
for %%n in (initbase inittop init_main initoff initexit initcrash) do (
  clang %CFLAGS% -O0 src\%%n.c -o obj\%%n.o || exit /b 1
)

rem ---- one: executable + one dylib (the example of the task) -------------------------------------------------
mkdir bin\one
%LD% -dylib -install_name @rpath/libmath.dylib -o bin\one\libmath.dylib obj\math.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\one -lmath -o bin\one\one obj\one_main.o -lSystem || exit /b 1

rem ---- two: executable + two dylibs ----------------------------------------------------------------------------
mkdir bin\two
%LD% -dylib -install_name @rpath/libmath.dylib -o bin\two\libmath.dylib obj\math.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libscale.dylib -o bin\two\libscale.dylib obj\scale.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\two -lmath -lscale -o bin\two\two obj\two_main.o -lSystem || exit /b 1

rem ---- trans: executable -> libtransa -> libtransb (transitive) ------------------------------------------------
mkdir bin\trans
%LD% -dylib -install_name @rpath/libtransb.dylib -o bin\trans\libtransb.dylib obj\trans_b.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libtransa.dylib -Lbin\trans -ltransb -o bin\trans\libtransa.dylib obj\trans_a.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\trans -ltransa -o bin\trans\trans obj\trans_main.o -lSystem || exit /b 1

rem ---- init: __mod_init_func in two dylibs and in the executable ---------------------------------------------------
mkdir bin\init
%LD% -dylib -install_name @rpath/libinitbase.dylib -o bin\init\libinitbase.dylib obj\initbase.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libinittop.dylib -Lbin\init -linitbase -o bin\init\libinittop.dylib obj\inittop.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\init -linitbase -linittop -o bin\init\init obj\init_main.o -lSystem || exit /b 1

rem ---- initoff: S_INIT_FUNC_OFFSETS (-init_offsets) ----------------------------------------------------------------
mkdir bin\initoff
%LD% -dylib -init_offsets -install_name @rpath/libinitoff.dylib -o bin\initoff\libinitoff.dylib obj\initoff.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\initoff -linitoff -o bin\initoff\initoff obj\initoff_main.o -lSystem || exit /b 1

rem ---- glob: an exported data symbol used in both directions -------------------------------------------------------
mkdir bin\glob
%LD% -dylib -install_name @rpath/libglob.dylib -o bin\glob\libglob.dylib obj\glob.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\glob -lglob -o bin\glob\glob obj\glob_main.o -lSystem || exit /b 1

rem ---- missing: the dependency is linked and then deleted ---------------------------------------------------------------
mkdir bin\missing
%LD% -dylib -install_name @rpath/libgone.dylib -o bin\missing\libgone.dylib obj\gone.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\missing -lgone -o bin\missing\missing obj\missing_main.o -lSystem || exit /b 1
copy /y bin\missing\libgone.dylib obj\libgone.dylib >nul
del bin\missing\libgone.dylib

rem ---- cycle: libcyca <-> libcycb (stub .tbd breaks the chicken-and-egg problem at link time) ----------------------------
mkdir bin\cycle
%LD% -dylib -install_name @rpath/libcycb.dylib -Ltbd -lcyca -o bin\cycle\libcycb.dylib obj\cycb.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libcyca.dylib -current_version 1.0.0 -compatibility_version 1.0.0 -Lbin\cycle -lcycb -o bin\cycle\libcyca.dylib obj\cyca.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\cycle -lcyca -o bin\cycle\cycle obj\cyc_main.o -lSystem || exit /b 1

rem ---- rpath: @rpath resolved through an LC_RPATH that points into a sub directory ------------------------------------
mkdir bin\rpath\lib
%LD% -dylib -install_name @rpath/librp.dylib -o bin\rpath\lib\librp.dylib obj\rp.o -lSystem || exit /b 1
%LD% -rpath @executable_path/lib -Lbin\rpath\lib -lrp -o bin\rpath\rpath obj\rp_main.o -lSystem || exit /b 1

rem ---- loaderpath: the inner dependency is relative to the OUTER library, not to the executable -----------------------
mkdir bin\loaderpath\libs\inner
%LD% -dylib -install_name @loader_path/inner/liblpinner.dylib -o bin\loaderpath\libs\inner\liblpinner.dylib obj\lpinner.o -lSystem || exit /b 1
%LD% -dylib -install_name @executable_path/libs/liblpouter.dylib -Lbin\loaderpath\libs\inner -llpinner -o bin\loaderpath\libs\liblpouter.dylib obj\lpouter.o -lSystem || exit /b 1
%LD% -Lbin\loaderpath\libs -llpouter -o bin\loaderpath\loaderpath obj\lp_main.o -lSystem || exit /b 1

rem ---- reexport: the executable binds core_fn through libfacade (LC_REEXPORT_DYLIB -> libcore) ----------------------------
mkdir bin\reexport
%LD% -dylib -install_name @rpath/libcore.dylib -o bin\reexport\libcore.dylib obj\core.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libfacade.dylib -Lbin\reexport -reexport-lcore -o bin\reexport\libfacade.dylib obj\facade.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\reexport -lfacade -o bin\reexport\reexport obj\reexp_main.o -lSystem || exit /b 1

rem ---- weak: LC_LOAD_WEAK_DYLIB whose library is not shipped; the weak import binds to NULL ---------------------------------
mkdir bin\weak
%LD% -dylib -install_name @rpath/libopt.dylib -o bin\weak\libopt.dylib obj\opt.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\weak -weak-lopt -o bin\weak\weak obj\weak_main.o -lSystem || exit /b 1
del bin\weak\libopt.dylib

rem ---- weakdef: weak definitions in the executable AND a dylib, coalesced through the weak-binding table (ORVYN 0.6) -----------------
mkdir bin\weakdef
%LD% -dylib -install_name @rpath/libwdef.dylib -o bin\weakdef\libwdef.dylib obj\wdef_lib.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\weakdef -lwdef -o bin\weakdef\weakdef obj\wdef_main.o -lSystem || exit /b 1

rem ---- weakdef2: the same weak variable in two dylibs, none in the executable: first library in load order wins ----------------------
clang %CFLAGS% -DVAL=7 -DGET=orvyn_a_get src\wdef2_lib.c -o obj\wdef2_a.o || exit /b 1
clang %CFLAGS% -DVAL=9 -DGET=orvyn_b_get src\wdef2_lib.c -o obj\wdef2_b.o || exit /b 1
mkdir bin\weakdef2
%LD% -dylib -install_name @rpath/libwa.dylib -o bin\weakdef2\libwa.dylib obj\wdef2_a.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libwb.dylib -o bin\weakdef2\libwb.dylib obj\wdef2_b.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\weakdef2 -lwa -lwb -o bin\weakdef2\weakdef2 obj\wdef2_main.o -lSystem || exit /b 1

rem ---- flat / flatmissing: -flat_namespace (symbols bound by name to "flat-namespace", no library recorded). ld64.lld needs the transitive libSystem.B.dylib
rem      on the search path for this mode: copy the sysroot to a temp directory and add usr\lib\libSystem.B.tbd (a copy of libSystem.tbd), then use it as -syslibroot.
rem      (commands used: %LD% -rpath @executable_path -Lbinlat -lmath -flat_namespace -o binlatlat obj\one_main.o -lSystem ;
rem       %LD% -undefined dynamic_lookup -flat_namespace -o binlatmissinglatmissing obj\missing_main.o -lSystem)

rem ---- initexit / initcrash: failing initializers -------------------------------------------------------------------------
mkdir bin\initexit
%LD% -dylib -install_name @rpath/libinitexit.dylib -o bin\initexit\libinitexit.dylib obj\initexit.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\initexit -linitexit -o bin\initexit\initexit obj\initexit_main.o -lSystem || exit /b 1
mkdir bin\initcrash
%LD% -dylib -install_name @rpath/libinitcrash.dylib -o bin\initcrash\libinitcrash.dylib obj\initcrash.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\initcrash -linitcrash -o bin\initcrash\initcrash obj\initcrash_main.o -lSystem || exit /b 1

rem ---- badver: the executable requires libmath >= 5.0.0 but the shipped library is 1.0.0 -------------------------------------
mkdir bin\badver
%LD% -dylib -install_name @rpath/libmath.dylib -compatibility_version 5.0.0 -current_version 5.0.0 -o bin\badver\libmath.dylib obj\math.o -lSystem || exit /b 1
%LD% -rpath @executable_path -Lbin\badver -lmath -o bin\badver\badver obj\badver_main.o -lSystem || exit /b 1
%LD% -dylib -install_name @rpath/libmath.dylib -current_version 1.0.0 -compatibility_version 1.0.0 -o bin\badver\libmath.dylib obj\math.o -lSystem || exit /b 1

rem ---- chained: LC_DYLD_CHAINED_FIXUPS (DYLD_CHAINED_PTR_64) in an executable and a dylib --------------------------------------
set "LDC=ld64.lld -arch x86_64 -platform_version macos 12.0.0 12.0.0 -fixup_chains -syslibroot %SYS%"
mkdir bin\chained
%LDC% -dylib -install_name @rpath/libchain.dylib -o bin\chained\libchain.dylib obj\chain_lib.o -lSystem || exit /b 1
%LDC% -rpath @executable_path -Lbin\chained -lchain -o bin\chained\chained obj\chain_main.o -lSystem || exit /b 1
mkdir bin\chainexe
%LDC% -o bin\chainexe\chainexe obj\chain_exe.o -lSystem || exit /b 1

rem ---- tls: Darwin x86_64 thread-local storage (descriptors bound to __tlv_bootstrap, no %gs in the guest) -------------------------
mkdir bin\tlsexe
%LD% -o bin\tlsexe\tlsexe obj\tls_exe.o -lSystem || exit /b 1
mkdir bin\tlslib
%LD% -dylib -install_name @rpath/libtls.dylib -o bin\tlslib\libtls.dylib obj\tls_lib.o -lSystem || exit /b 1
%LD% -rpath @executable_path -L bin\tlslib -ltls -o bin\tlslib\tlslib obj\tls_main.o -lSystem || exit /b 1

rem ---- snapshots of every Mach-O produced ----------------------------------------------------------------------------------------
for /r bin %%f in (*) do (
  set "N=%%~nxf"
  set "D=%%~dpf"
  set "REL=!D:%R%\bin\=!"
  set "KEY=!REL:\=_!!N!"
  llvm-objdump --macho --private-headers "%%f" > "golden\!KEY!.private-headers.txt" || exit /b 1
  llvm-objdump --macho --exports-trie "%%f" > "golden\!KEY!.exports.txt" || exit /b 1
  llvm-objdump --macho --syms "%%f" > "golden\!KEY!.syms.txt" || exit /b 1
  llvm-objdump --macho --dyld-info "%%f" > "golden\!KEY!.dyldinfo.txt" || exit /b 1
  llvm-objdump --macho --chained-fixups "%%f" > "golden\!KEY!.chained.txt" || exit /b 1
  llvm-objdump --macho --rebase --bind --lazy-bind "%%f" > "golden\!KEY!.fixups.txt" || exit /b 1
)
llvm-objdump --version | findstr /b /c:"  LLVM version" > golden\TOOL-VERSION.txt
clang --version | findstr /b /c:"clang version" >> golden\TOOL-VERSION.txt
ld64.lld --version >> golden\TOOL-VERSION.txt
rmdir /s /q obj
popd
echo done
exit /b 0
