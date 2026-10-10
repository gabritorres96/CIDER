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
    abi/sysv_bridge.cpp abi/callback_bridge.cpp abi/stack_builder.cpp abi/entry_trampoline.asm
    libsystem/libc_bridge.cpp libsystem/process_runtime.cpp libsystem/stdio.cpp libsystem/stdio_streams.cpp libsystem/posix.cpp libsystem/misc.cpp objc/objc_runtime.cpp objc/objc_memory.cpp objc/objc_metadata.cpp objc/objc_dispatch.cpp objc/objc_api.cpp objc/objc_msgsend.asm foundation/foundation.cpp foundation/nsobject.cpp foundation/nsstring.cpp foundation/nsnumber.cpp foundation/nsarray.cpp foundation/nsdictionary.cpp foundation/nsextras.cpp foundation/nsnotification.cpp libsystem/guest_threads.cpp libsystem/pthread_bridge.cpp libsystem/callbacks.cpp libsystem/blocks.cpp dispatch/dispatch.cpp graphics/cg_bridge.cpp corefoundation/cf_bridge.cpp corefoundation/runloop.cpp appkit/appkit_core.cpp appkit/appkit_views.cpp appkit/appkit_window.cpp appkit/appkit_app.cpp appkit/appkit_event.cpp graphics/win32_backend.cpp graphics/offscreen_backend.cpp libsystem/tls.cpp libsystem/printf_format.cpp
    process/guest_process.cpp process/exit_handler.cpp process/supervisor.cpp process/worker.cpp)
  target_link_libraries(cider_runtime PUBLIC cider_core cider_macho cider_dyld cider_geometry)
endif()

cider_lib(cider_sdk sdk/tbd.cpp)
cider_lib(cider_geometry graphics/geometry.cpp)
add_executable(cider cli/main.cpp cli/commands.cpp cli/run_command.cpp cli/sdk_command.cpp)
if(MSVC)
  target_sources(cider PRIVATE cli/cider.manifest)
endif()
target_link_libraries(cider PRIVATE cider_analysis cider_dmg cider_sdk)
if(CIDER_HAS_RUNTIME)
  target_link_libraries(cider PRIVATE cider_runtime)
endif()
cider_target_options(cider)
if(WIN32)
  target_link_libraries(cider PRIVATE shell32)
endif()
