cmake_minimum_required(VERSION 3.15)

project(libskyrc C)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

set(LIBSKYRC_VERSION "" CACHE STRING "Override the library version (X.Y.Z)")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi-hidraw)

find_package(Git QUIET)

set(LIBSKYRC_GIT_TAG "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git" AND Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE LIBSKYRC_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(LIBSKYRC_VERSION STREQUAL "" AND DEFINED ENV{LIBSKYRC_VERSION} AND NOT "$ENV{LIBSKYRC_VERSION}" STREQUAL "")
    set(LIBSKYRC_VERSION "$ENV{LIBSKYRC_VERSION}")
endif()
if(LIBSKYRC_VERSION STREQUAL "")
    if(LIBSKYRC_GIT_TAG STREQUAL "")
        set(LIBSKYRC_VERSION "1.0.0")
    else()
        string(REGEX REPLACE "^v" "" LIBSKYRC_VERSION "${LIBSKYRC_GIT_TAG}")
    endif()
endif()
if(NOT LIBSKYRC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Version '${LIBSKYRC_VERSION}' must be a semantic version (for example, 1.0.0)")
endif()
string(REGEX REPLACE "^([0-9]+\\.[0-9]+)\\.[0-9]+$" "\\1" LIBSKYRC_SOVERSION "${LIBSKYRC_VERSION}")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

include_directories(PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(libskyrc-usb SHARED
    src/skyrc-usb.c
    include/skyrc-usb.h
)
target_compile_definitions(libskyrc-usb PRIVATE libskyrc_usb_EXPORTS)
target_link_libraries(libskyrc-usb PRIVATE PkgConfig::HIDAPI)
target_compile_options(libskyrc-usb PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wcast-qual
        -Wcast-align
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wmissing-declarations
        -Wredundant-decls
        -Wnull-dereference
        -Wformat=2
        -Wformat-overflow=2
        -Wformat-truncation=2
        -Wvla
        -Wwrite-strings
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wundef
        -Wuninitialized
        -Winit-self
        -Wlogical-op
        -Werror
    >
)

set_target_properties(libskyrc-usb PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/skyrc-usb.h"
    INTERPROCEDURAL_OPTIMIZATION TRUE
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME "skyrc-usb"
    VERSION "${LIBSKYRC_VERSION}"
    SOVERSION "${LIBSKYRC_SOVERSION}"
)

add_executable(skyrc
    src/main.c
)
set_target_properties(skyrc PROPERTIES OUTPUT_NAME "skyrc-usb")
target_link_libraries(skyrc PRIVATE libskyrc-usb)
target_compile_options(skyrc PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wcast-qual
        -Wcast-align
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wmissing-declarations
        -Wredundant-decls
        -Wnull-dereference
        -Wformat=2
        -Wformat-overflow=2
        -Wformat-truncation=2
        -Wvla
        -Wwrite-strings
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wundef
        -Wuninitialized
        -Winit-self
        -Wlogical-op
        -Werror
    >
)

include(GNUInstallDirs)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(UDEV QUIET udev)
    if(UDEV_FOUND)
        pkg_get_variable(UDEV_RULES_DIR udev udevdir)
        if(UDEV_RULES_DIR)
            set(UDEV_RULES_DIR "${UDEV_RULES_DIR}/rules.d")
        endif()
    endif()
endif()

if(NOT UDEV_RULES_DIR)
    set(UDEV_RULES_DIR "${CMAKE_INSTALL_LIBDIR}/udev/rules.d")
endif()

install(TARGETS libskyrc-usb skyrc
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES 99-skyrc.rules
    DESTINATION ${UDEV_RULES_DIR}
)


