add_library(xenia-cpu STATIC)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR})
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/backend)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler/passes)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/hir)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/ppc)
set_property(SOURCE
  ${CMAKE_CURRENT_SOURCE_DIR}/guest_invocation_replay_config.cc
  PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Release>:XE_BUILD_RELEASE=1>
    $<$<AND:$<CONFIG:Release>,$<BOOL:${XENIA_ENABLE_LTO}>>:XE_BUILD_LTO=1>
)
target_include_directories(xenia-cpu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/llvm/include
)
target_link_libraries(xenia-cpu PUBLIC xenia-base mspack)
xe_target_defaults(xenia-cpu)

if(XENIA_BUILD_TESTS)
  add_subdirectory(testing)
  add_subdirectory(ppc/testing)
endif()
