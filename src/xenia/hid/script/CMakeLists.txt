add_library(xenia-hid-script STATIC)
xe_platform_sources(xenia-hid-script ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-script PUBLIC xenia-base xenia-hid)
xe_target_defaults(xenia-hid-script)
