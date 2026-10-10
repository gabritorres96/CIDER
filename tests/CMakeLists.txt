add_library(cider_test_main STATIC unit/test_main.cpp)
target_include_directories(cider_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unit ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
target_link_libraries(cider_test_main PUBLIC cider_analysis cider_dmg)
cider_target_options(cider_test_main)

# cider_test(<name> <sources...>): one executable + one CTest entry.
function(cider_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE cider_test_main)
  cider_target_options(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

cider_test(test_util unit/test_util.cpp)
cider_test(test_macho unit/test_macho.cpp unit/test_macho_more.cpp)
cider_test(test_malformed unit/test_malformed.cpp)
cider_test(test_fat unit/test_fat.cpp)
cider_test(test_plist unit/test_plist.cpp)
cider_test(test_bundle unit/test_bundle.cpp)
cider_test(test_mutation_fuzz regression/test_mutation_fuzz.cpp)
cider_test(test_dmg unit/test_dmg.cpp)

# Integration tests drive the real cider executable.
add_executable(test_cli integration/test_cli.cpp)
target_link_libraries(test_cli PRIVATE cider_test_main)
cider_target_options(test_cli)
add_dependencies(test_cli cider)
add_test(NAME test_cli COMMAND test_cli)
set_tests_properties(test_cli PROPERTIES ENVIRONMENT "CIDER_EXE=$<TARGET_FILE:cider>")

# Real LLVM-built Mach-O fixtures compared against llvm-objdump snapshots (tests/fixtures/real).
cider_test(test_real_macho regression/test_real_macho.cpp)
target_compile_definitions(test_real_macho PRIVATE CIDER_REAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/real")
target_compile_definitions(test_cli PRIVATE CIDER_REAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/real")
