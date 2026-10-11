add_library(orvyn_test_main STATIC unit/test_main.cpp)
target_include_directories(orvyn_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unit ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
target_link_libraries(orvyn_test_main PUBLIC orvyn_analysis orvyn_dmg orvyn_sdk)
if(ORVYN_HAS_RUNTIME)
  target_link_libraries(orvyn_test_main PUBLIC orvyn_runtime)
endif()
orvyn_target_options(orvyn_test_main)

# orvyn_test(<name> <sources...>): one executable + one CTest entry.
function(orvyn_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE orvyn_test_main)
  orvyn_target_options(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

orvyn_test(test_util unit/test_util.cpp)
orvyn_test(test_macho unit/test_macho.cpp unit/test_macho_more.cpp)
orvyn_test(test_malformed unit/test_malformed.cpp)
orvyn_test(test_fat unit/test_fat.cpp)
orvyn_test(test_plist unit/test_plist.cpp)
orvyn_test(test_sdk unit/test_sdk.cpp)
orvyn_test(test_geometry unit/test_geometry.cpp)
target_link_libraries(test_geometry PRIVATE orvyn_geometry)
orvyn_test(test_cg unit/test_cg.cpp)
target_link_libraries(test_cg PRIVATE orvyn_cg)
orvyn_test(test_bundle unit/test_bundle.cpp)
orvyn_test(test_mutation_fuzz regression/test_mutation_fuzz.cpp)
orvyn_test(test_dmg unit/test_dmg.cpp)

# Integration tests drive the real orvyn executable.
add_executable(test_cli integration/test_cli.cpp)
target_link_libraries(test_cli PRIVATE orvyn_test_main)
orvyn_target_options(test_cli)
add_dependencies(test_cli orvyn)
add_test(NAME test_cli COMMAND test_cli)

# Interactive GUI tests: the real orvyn.exe opens a real window, this process drives it with window messages. Skipped without an interactive desktop.
add_executable(test_gui_win32 integration/test_gui_win32.cpp)
target_link_libraries(test_gui_win32 PRIVATE orvyn_test_main)
orvyn_target_options(test_gui_win32)
target_compile_definitions(test_gui_win32 PRIVATE ORVYN_APPKIT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/appkit")
add_dependencies(test_gui_win32 orvyn)
add_test(NAME test_gui_win32 COMMAND test_gui_win32)
set_tests_properties(test_gui_win32 PROPERTIES ENVIRONMENT "ORVYN_EXE=$<TARGET_FILE:orvyn>" LABELS gui)
set_tests_properties(test_cli PROPERTIES ENVIRONMENT "ORVYN_EXE=$<TARGET_FILE:orvyn>")

# Real LLVM-built Mach-O fixtures compared against llvm-objdump snapshots (tests/fixtures/real).
orvyn_test(test_real_macho regression/test_real_macho.cpp)
target_compile_definitions(test_real_macho PRIVATE ORVYN_REAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/real")
target_compile_definitions(test_cli PRIVATE ORVYN_REAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/real"
  ORVYN_EXEC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exec"
  ORVYN_DYLIB_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dylib"
  ORVYN_OBJC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/objc"
  ORVYN_APPKIT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/appkit")

# ---- ORVYN 0.2 runtime: real Mach-O execution (Windows x64 only) ---------------------------------------------------------
if(ORVYN_HAS_RUNTIME)
  function(orvyn_runtime_test name)
    orvyn_test(${name} ${ARGN})
    target_compile_definitions(${name} PRIVATE
      ORVYN_EXEC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exec"
      ORVYN_DYLIB_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dylib"
      ORVYN_OBJC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/objc"
      ORVYN_APPKIT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/appkit"
      ORVYN_REAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/real")
  endfunction()
  orvyn_runtime_test(test_memory runtime/test_memory.cpp)
  orvyn_runtime_test(test_abi runtime/test_abi.cpp runtime/test_abi_raw.cpp runtime/abi_selftest.asm)
  orvyn_runtime_test(test_loader runtime/test_loader.cpp)
  orvyn_runtime_test(test_entry runtime/test_entry.cpp)
  orvyn_runtime_test(test_linker runtime/test_linker.cpp)
  orvyn_runtime_test(test_darwin_runtime runtime/test_darwin_runtime.cpp)
  orvyn_runtime_test(test_chained_fixups runtime/test_chained_fixups.cpp)
  orvyn_runtime_test(test_tls runtime/test_tls.cpp)
  orvyn_runtime_test(test_callbacks runtime/test_callbacks.cpp)
  orvyn_runtime_test(test_streams runtime/test_streams.cpp)
  orvyn_runtime_test(test_threads runtime/test_threads.cpp)
  orvyn_runtime_test(test_objc runtime/test_objc.cpp)
  orvyn_runtime_test(test_foundation runtime/test_foundation.cpp)
  orvyn_runtime_test(test_blocks runtime/test_blocks.cpp)
  orvyn_runtime_test(test_dispatch runtime/test_dispatch.cpp)
  orvyn_runtime_test(test_foundation_extra runtime/test_foundation_extra.cpp)
  orvyn_runtime_test(test_corefoundation runtime/test_corefoundation.cpp)
  orvyn_runtime_test(test_appkit runtime/test_appkit.cpp)
  orvyn_runtime_test(test_cg_rendering runtime/test_cg_rendering.cpp)
  orvyn_runtime_test(test_cg_abi runtime/test_cg_abi.cpp)
  orvyn_runtime_test(test_objc_dynamic runtime/test_objc_dynamic.cpp)
  orvyn_runtime_test(test_frameworks runtime/test_frameworks.cpp)
  add_dependencies(test_threads orvyn)
  set_tests_properties(test_threads PROPERTIES ENVIRONMENT "ORVYN_EXE=$<TARGET_FILE:orvyn>")
  orvyn_runtime_test(test_symbols runtime/test_symbols.cpp)
  orvyn_runtime_test(test_runtime runtime/test_runtime.cpp)
  orvyn_runtime_test(test_execution runtime/test_execution.cpp)
  orvyn_runtime_test(test_execution_errors runtime/test_execution_errors.cpp)
  add_dependencies(test_execution_errors orvyn)
  set_tests_properties(test_execution_errors PROPERTIES ENVIRONMENT "ORVYN_EXE=$<TARGET_FILE:orvyn>")
endif()
