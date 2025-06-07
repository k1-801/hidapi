cmake_minimum_required(VERSION 3.1.3...3.25 FATAL_ERROR)
project(hidtest_interactive C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # hidtest_interactive is build as a standalone project

    if(POLICY CMP0074)
        # allow using hidapi_ROOT if CMake supports it
        cmake_policy(SET CMP0074 NEW)
    endif()

    find_package(hidapi 0.12 REQUIRED)
    message(STATUS "Using HIDAPI: ${hidapi_VERSION}")
else()
    # hidtest_interactive is built as part of the main HIDAPI build
    message(STATUS "Building hidtest_interactive")
endif()

set(HIDAPI_HIDTEST_INTERACTIVE_TARGETS)
if(NOT WIN32 AND NOT APPLE AND CMAKE_SYSTEM_NAME MATCHES "Linux")
    if(TARGET hidapi::hidraw)
        add_executable(hidtest_interactive_hidraw test.c)
        target_link_libraries(hidtest_interactive_hidraw hidapi::hidraw)
        list(APPEND HIDAPI_HIDTEST_INTERACTIVE_TARGETS hidtest_interactive_hidraw)
    endif()
    if(TARGET hidapi::libusb)
        add_executable(hidtest_interactive_libusb test.c)
        target_compile_definitions(hidtest_interactive_libusb PRIVATE USING_HIDAPI_LIBUSB)
        target_link_libraries(hidtest_interactive_libusb hidapi::libusb)
        list(APPEND HIDAPI_HIDTEST_INTERACTIVE_TARGETS hidtest_interactive_libusb)
    endif()
else()
    add_executable(hidtest_interactive test.c)
    target_link_libraries(hidtest_interactive hidapi::hidapi)
    list(APPEND HIDAPI_HIDTEST_INTERACTIVE_TARGETS hidtest_interactive)
endif()

install(TARGETS ${HIDAPI_HIDTEST_INTERACTIVE_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
