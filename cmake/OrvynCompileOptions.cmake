# Applies warnings (and optional sanitizers) to a target.
#
# AddressSanitizer on Windows:
#  * MSVC cl.exe: needs the "C++ AddressSanitizer" Visual Studio component (/fsanitize=address).
#  * clang-cl (e.g. the portable LLVM in .toolchain): needs the release DLL CRT (set in the root
#    CMakeLists) and the clang_rt.asan_dynamic runtime libraries, added explicitly below because
#    CMake invokes the linker directly. At run time clang_rt.asan_dynamic-x86_64.dll must be on PATH.

set(ORVYN_CLANG_RT_DIR "" CACHE PATH "Directory with clang_rt.*.lib (auto-detected for clang-cl)")
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT ORVYN_CLANG_RT_DIR)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
                  OUTPUT_VARIABLE _orvyn_res OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_orvyn_res)
    file(TO_CMAKE_PATH "${_orvyn_res}/lib/windows" _orvyn_rt)
    set(ORVYN_CLANG_RT_DIR "${_orvyn_rt}" CACHE PATH "Directory with clang_rt.*.lib" FORCE)
  endif()
endif()

function(orvyn_link_asan target)
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND ORVYN_CLANG_RT_DIR)
    target_link_directories(${target} PRIVATE ${ORVYN_CLANG_RT_DIR})
    target_link_libraries(${target} PRIVATE clang_rt.asan_dynamic-x86_64 clang_rt.asan_dynamic_runtime_thunk-x86_64)
    target_link_options(${target} PRIVATE /WHOLEARCHIVE:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
  endif()
endfunction()

function(orvyn_target_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus>)
    target_compile_definitions(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:_CRT_SECURE_NO_WARNINGS NOMINMAX>)
    if(ORVYN_SANITIZE)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address /Zi>)
      # STL container annotations need the VS ASan component's stl_asan.lib; not required here.
      target_compile_definitions(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:_DISABLE_STL_ANNOTATION>)
      if(ORVYN_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # coverage instrumentation in the library code under test, otherwise libFuzzer is blind
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=fuzzer-no-link>)
      endif()
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
      get_target_property(_type ${target} TYPE)
      if(_type STREQUAL "EXECUTABLE")
        orvyn_link_asan(${target})
      endif()
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(ORVYN_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()
