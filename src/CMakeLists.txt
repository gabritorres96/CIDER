function(orvyn_lib name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/include)
  orvyn_target_options(${name})
endfunction()

orvyn_lib(orvyn_core core/bytes.cpp core/util.cpp core/json.cpp core/process.cpp logging/logger.cpp)
orvyn_lib(orvyn_plist corefoundation/plist.cpp)
target_link_libraries(orvyn_plist PUBLIC orvyn_core)
orvyn_lib(orvyn_macho macho/macho.cpp macho/names.cpp macho/codesign.cpp)
target_link_libraries(orvyn_macho PUBLIC orvyn_core)
orvyn_lib(orvyn_dyld dyld/install_name.cpp dyld/relocation_engine.cpp dyld/symbol_resolver.cpp dyld/export_trie.cpp dyld/chained_fixups.cpp)

target_link_libraries(orvyn_dyld PUBLIC orvyn_core orvyn_macho)
orvyn_lib(orvyn_bundle bundle/bundle.cpp)
target_link_libraries(orvyn_bundle PUBLIC orvyn_core orvyn_macho orvyn_plist)
orvyn_lib(orvyn_analysis analysis/analysis.cpp)
target_link_libraries(orvyn_analysis PUBLIC orvyn_core orvyn_macho orvyn_dyld orvyn_bundle)

orvyn_lib(orvyn_dmg dmg/udif.cpp dmg/sevenzip.cpp)
target_link_libraries(orvyn_dmg PUBLIC orvyn_core orvyn_plist)

if(ORVYN_HAS_RUNTIME)
  # ORVYN 0.2 runtime: memory, loader, validator, ABI bridge, libSystem subset, guest process management
  orvyn_lib(orvyn_runtime
    memory/virtual_memory.cpp memory/memory_protection.cpp
    security/execution_validator.cpp
    loader/image_layout.cpp loader/segment_mapper.cpp loader/macho_loader.cpp
    linker/linker.cpp
    abi/sysv_bridge.cpp abi/callback_bridge.cpp abi/stack_builder.cpp abi/entry_trampoline.asm
    libsystem/libc_bridge.cpp libsystem/process_runtime.cpp libsystem/stdio.cpp libsystem/stdio_streams.cpp libsystem/posix.cpp libsystem/misc.cpp objc/objc_runtime.cpp objc/objc_memory.cpp objc/objc_metadata.cpp objc/objc_dispatch.cpp objc/objc_api.cpp objc/objc_msgsend.asm foundation/foundation.cpp foundation/nsobject.cpp foundation/nsstring.cpp foundation/nsnumber.cpp foundation/nsarray.cpp foundation/nsdictionary.cpp foundation/nsextras.cpp foundation/nsnotification.cpp foundation/nsinvocation.cpp foundation/nsexception.cpp libsystem/guest_threads.cpp libsystem/pthread_bridge.cpp libsystem/callbacks.cpp libsystem/blocks.cpp dispatch/dispatch.cpp graphics/cg_bridge.cpp graphics/cg_api.cpp corefoundation/cf_bridge.cpp corefoundation/runloop.cpp appkit/appkit_core.cpp appkit/appkit_views.cpp appkit/appkit_window.cpp appkit/appkit_app.cpp appkit/appkit_event.cpp appkit/appkit_draw.cpp graphics/win32_backend.cpp graphics/offscreen_backend.cpp libsystem/tls.cpp libsystem/printf_format.cpp
    process/guest_process.cpp process/exit_handler.cpp process/supervisor.cpp process/worker.cpp)
  target_link_libraries(orvyn_runtime PUBLIC orvyn_core orvyn_macho orvyn_dyld orvyn_geometry orvyn_cg)
endif()

orvyn_lib(orvyn_sdk sdk/tbd.cpp)
orvyn_lib(orvyn_geometry graphics/geometry.cpp)
orvyn_lib(orvyn_cg graphics/cg_context.cpp)
target_link_libraries(orvyn_cg PUBLIC orvyn_geometry)
if(WIN32)
  target_sources(orvyn_cg PRIVATE graphics/d2d_device.cpp)
  target_link_libraries(orvyn_cg PUBLIC d2d1 dwrite windowscodecs ole32 uuid)
endif()
add_executable(orvyn cli/main.cpp cli/commands.cpp cli/run_command.cpp cli/sdk_command.cpp)
if(MSVC)
  target_sources(orvyn PRIVATE cli/orvyn.manifest)
endif()
target_link_libraries(orvyn PRIVATE orvyn_analysis orvyn_dmg orvyn_sdk)
if(ORVYN_HAS_RUNTIME)
  target_link_libraries(orvyn PRIVATE orvyn_runtime)
endif()
orvyn_target_options(orvyn)
if(WIN32)
  target_link_libraries(orvyn PRIVATE shell32)
endif()
