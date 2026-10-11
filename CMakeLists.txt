cmake_minimum_required(VERSION 3.21)
project(ORVYN VERSION 0.5.0 LANGUAGES CXX)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  enable_language(ASM_MASM)   # entry_trampoline.asm (System V <-> Windows x64 bridge)
  set(ORVYN_HAS_RUNTIME ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ORVYN_BUILD_TESTS "Build the test suite" ON)
option(ORVYN_BUILD_FUZZERS "Build libFuzzer harnesses (needs MSVC /fsanitize=fuzzer or clang)" OFF)
option(ORVYN_SANITIZE "Enable AddressSanitizer where the compiler supports it" OFF)

if(ORVYN_SANITIZE AND MSVC)
  # clang-cl ASan supports only the release CRTs; the DLL CRT avoids stl_asan.lib (VS ASan component). Needs clang_rt.asan_dynamic-x86_64.dll on PATH to run.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

include(cmake/OrvynCompileOptions.cmake)

add_subdirectory(src)

if(ORVYN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(ORVYN_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()

if(ORVYN_BUILD_TESTS)
  add_subdirectory(tools)
endif()
