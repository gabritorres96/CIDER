# libFuzzer harnesses (optional; not built by default).
#
# Windows + clang-cl note: the prebuilt clang_rt.fuzzer-x86_64.lib is built against the static CRT
# (/MT) but Windows ASan requires the DLL CRT (/MD), so linking them fails with a RuntimeLibrary
# mismatch. Workaround: build libFuzzer from its sources (llvm-project/compiler-rt/lib/fuzzer,
# Apache-2.0 WITH LLVM-exception) with /MD. Point ORVYN_LIBFUZZER_SRC at that directory:
#   git clone --depth 1 --branch llvmorg-23.1.3 --filter=blob:none --sparse https://github.com/llvm/llvm-project.git
#   git sparse-checkout set compiler-rt/lib/fuzzer
set(ORVYN_LIBFUZZER_SRC "" CACHE PATH "compiler-rt/lib/fuzzer source directory (clang-cl builds)")

if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(NOT ORVYN_LIBFUZZER_SRC)
    message(FATAL_ERROR "Set -DORVYN_LIBFUZZER_SRC=<llvm-project>/compiler-rt/lib/fuzzer (see tests/fuzz/CMakeLists.txt)")
  endif()
  file(GLOB _lf_sources ${ORVYN_LIBFUZZER_SRC}/Fuzzer*.cpp)
  list(FILTER _lf_sources EXCLUDE REGEX "Darwin|Fuchsia|Linux|Posix|Dlsym|Weak|Interceptors|FuzzerMain")
  add_library(orvyn_libfuzzer STATIC ${_lf_sources} ${ORVYN_LIBFUZZER_SRC}/FuzzerMain.cpp)
  target_compile_options(orvyn_libfuzzer PRIVATE -fno-sanitize=all /w)
  target_compile_definitions(orvyn_libfuzzer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

foreach(name fuzz_macho fuzz_plist fuzz_udif fuzz_loader fuzz_chained fuzz_linker fuzz_objc fuzz_tbd fuzz_cg)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE orvyn_macho orvyn_plist orvyn_dmg orvyn_sdk)
  if(ORVYN_HAS_RUNTIME)
    target_link_libraries(${name} PRIVATE orvyn_runtime)
  endif()
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer-no-link /fsanitize=address /Zi)
    target_compile_definitions(${name} PRIVATE _DISABLE_STL_ANNOTATION)
    target_link_libraries(${name} PRIVATE orvyn_libfuzzer)
    orvyn_link_asan(${name})
  elseif(MSVC)
    target_compile_options(${name} PRIVATE /fsanitize=fuzzer /fsanitize=address)
  else()
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address)
  endif()
endforeach()
