add_library(xenia-cpu STATIC)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR})
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/backend)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler/passes)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/hir)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/ppc)
target_include_directories(xenia-cpu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/llvm/include
)
if(NOT XENIA_BUILD_EMULATOR)
  # xex_module.cc is the only file in xenia-cpu that reaches into xenia/kernel.
  # Without the emulator nothing ever loads a XEX - the PPC test runner uses
  # RawModule - so drop it and keep kernel headers out of this library.
  get_target_property(_cpu_srcs xenia-cpu SOURCES)
  list(FILTER _cpu_srcs EXCLUDE REGEX "xex_module\\.(cc|h)$")
  set_target_properties(xenia-cpu PROPERTIES SOURCES "${_cpu_srcs}")
endif()

target_link_libraries(xenia-cpu PUBLIC xenia-base mspack)
xe_target_defaults(xenia-cpu)

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
  add_subdirectory(ppc/testing)
endif()
