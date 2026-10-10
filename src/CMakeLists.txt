function(cider_lib name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/include)
  cider_target_options(${name})
endfunction()

cider_lib(cider_core core/bytes.cpp core/util.cpp core/json.cpp core/process.cpp logging/logger.cpp)
cider_lib(cider_plist corefoundation/plist.cpp)
target_link_libraries(cider_plist PUBLIC cider_core)
cider_lib(cider_macho macho/macho.cpp macho/names.cpp macho/codesign.cpp)
target_link_libraries(cider_macho PUBLIC cider_core)
cider_lib(cider_dyld dyld/install_name.cpp dyld/relocation_engine.cpp dyld/symbol_resolver.cpp dyld/export_trie.cpp dyld/chained_fixups.cpp)

target_link_libraries(cider_dyld PUBLIC cider_core cider_macho)
cider_lib(cider_bundle bundle/bundle.cpp)
target_link_libraries(cider_bundle PUBLIC cider_core cider_macho cider_plist)
cider_lib(cider_analysis analysis/analysis.cpp)
target_link_libraries(cider_analysis PUBLIC cider_core cider_macho cider_dyld cider_bundle)

cider_lib(cider_dmg dmg/udif.cpp dmg/sevenzip.cpp)
target_link_libraries(cider_dmg PUBLIC cider_core cider_plist)

if(CIDER_HAS_RUNTIME)
  # CIDER 0.2 runtime: memory, loader, validator, ABI bridge, libSystem subset, guest process management
  cider_lib(cider_runtime
    memory/virtual_memory.cpp memory/memory_protection.cpp
    security/execution_validator.cpp
    loader/image_layout.cpp loader/segment_mapper.cpp loader/macho_loader.cpp
    linker/linker.cpp
    abi/sysv_bridge.cpp abi/stack_builder.cpp abi/entry_trampoline.asm
    libsystem/libc_bridge.cpp libsystem/process_runtime.cpp libsystem/stdio.cpp libsystem/posix.cpp libsystem/misc.cpp libsystem/tls.cpp libsystem/printf_format.cpp
    process/guest_process.cpp process/exit_handler.cpp process/supervisor.cpp process/worker.cpp)
  target_link_libraries(cider_runtime PUBLIC cider_core cider_macho cider_dyld)
endif()

add_executable(cider cli/main.cpp cli/commands.cpp cli/run_command.cpp)
target_link_libraries(cider PRIVATE cider_analysis cider_dmg)
if(CIDER_HAS_RUNTIME)
  target_link_libraries(cider PRIVATE cider_runtime)
endif()
cider_target_options(cider)
if(WIN32)
  target_link_libraries(cider PRIVATE shell32)
endif()
