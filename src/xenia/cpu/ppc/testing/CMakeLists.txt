if(NOT XENIA_BUILD_TESTS)
  return()
endif()

add_executable(xenia-cpu-ppc-tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ppc_testing_main.cc
)

target_compile_definitions(xenia-cpu-ppc-tests PRIVATE
    XE_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")

# Add platform-specific console app main
if(WIN32)
  target_sources(xenia-cpu-ppc-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_win.cc)
else()
  target_sources(xenia-cpu-ppc-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc)
endif()

# Attach the PPC assembly test inputs to the target for IDE visibility;
# they're test data, not compiled code.
file(GLOB _ppc_asm "${CMAKE_CURRENT_SOURCE_DIR}/*.s")
target_sources(xenia-cpu-ppc-tests PRIVATE ${_ppc_asm})
set_source_files_properties(${_ppc_asm} PROPERTIES HEADER_FILE_ONLY TRUE)

# The driver includes only xenia/base/* and xenia/cpu/*, and nothing under
# src/xenia/cpu includes xenia/ui. The emulator subsystems were linked here
# defensively, not because the binary needs them, and xenia-kernel is what
# drags in xenia-ui -> GTK3/fontconfig on Linux.
if(XENIA_BUILD_EMULATOR)
  target_link_libraries(xenia-cpu-ppc-tests PRIVATE
    xenia-apu xenia-apu-nop
    xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-null
    xenia-hid xenia-hid-nop
    xenia-kernel xenia-patcher xenia-ui xenia-vfs
    capstone fmt imgui mspack
  )
else()
  # Processor::LookupModule does dynamic_cast<XexModule*>, so xex_module.o is
  # pulled out of libxenia-cpu.a and its kernel references must still resolve.
  # ppc_testing_stubs.cc supplies them; none of them run, because the driver
  # only ever loads a RawModule.
  target_sources(xenia-cpu-ppc-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ppc_testing_stubs.cc)
  target_link_libraries(xenia-cpu-ppc-tests PRIVATE
    xenia-base xenia-core xenia-cpu
    capstone fmt mspack
  )
endif()

# Architecture-specific backend
if(XE_TARGET_X86_64)
  target_link_libraries(xenia-cpu-ppc-tests PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  target_link_libraries(xenia-cpu-ppc-tests PRIVATE xenia-cpu-backend-a64)
endif()

# See src/xenia/apu/CMakeLists.txt — xenia-apu requires xenia-apu-sdl on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND XENIA_BUILD_EMULATOR)
  target_link_libraries(xenia-cpu-ppc-tests PRIVATE xenia-apu-sdl)
endif()

xe_target_defaults(xenia-cpu-ppc-tests)
