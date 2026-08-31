cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(nrpc VERSION 1.0.0 LANGUAGES C CXX)

include(FetchContent)

set(NRPC_LIBUV_VERSION "1.52.1")
set(NRPC_MBEDTLS_VERSION "3.6.6")
set(NRPC_GOOGLETEST_VERSION "1.17.0")

if(WIN32)
    set(NRPC_DEFAULT_PLATFORM windows)
else()
    set(NRPC_DEFAULT_PLATFORM linux)
endif()
set(
    PLATFORM
    "${NRPC_DEFAULT_PLATFORM}"
    CACHE STRING "Target platform: windows or linux"
)
set_property(CACHE PLATFORM PROPERTY STRINGS windows linux)
string(TOLOWER "${PLATFORM}" PLATFORM)
if(NOT PLATFORM MATCHES "^(windows|linux)$")
    message(FATAL_ERROR "PLATFORM must be either 'windows' or 'linux'")
endif()
if(PLATFORM STREQUAL "windows" AND NOT WIN32)
    message(FATAL_ERROR "PLATFORM=windows requires a Windows build host/toolchain")
endif()
if(PLATFORM STREQUAL "linux" AND WIN32)
    message(FATAL_ERROR "PLATFORM=linux requires a Linux build host/toolchain")
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
        message(FATAL_ERROR "CMAKE_BUILD_TYPE must be Debug or Release")
    endif()
endif()

if(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od /Ob0 /RTC1" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO" CACHE STRING "" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "/INCREMENTAL:NO" CACHE STRING "" FORCE)
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
endif()

set(
    NRPC_PYTHON_CLIENT_SOURCE_DIR
    ""
    CACHE PATH "Path to the standalone nrpc-python-client repository"
)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG "v${NRPC_LIBUV_VERSION}"
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(libuv)
set(NRPC_LIBUV_TARGET uv_a)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    mbedtls
    URL
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${NRPC_MBEDTLS_VERSION}/mbedtls-${NRPC_MBEDTLS_VERSION}.tar.bz2"
    URL_HASH
        SHA256=8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a
)
FetchContent_MakeAvailable(mbedtls)
set(NRPC_MBEDTLS_TARGET MbedTLS::mbedtls)

function(nrpc_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            ${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
endfunction()

add_library(nrpc
    src/arena.cpp
    src/dispatcher.cpp
    src/gpio_backend.cpp
    src/lcard_backend.cpp
    src/logger.cpp
    src/nidaqmx_backend.cpp
    src/protocol.cpp
    src/resource_manager.cpp
    src/serial_backend.cpp
    src/server.cpp
    src/tcp_backend.cpp
)
if(PLATFORM STREQUAL "windows")
    target_sources(nrpc PRIVATE src/serial_port_win32.cpp src/lcard_native_win32.cpp)
else()
    target_sources(nrpc PRIVATE src/serial_port_posix.cpp)
endif()

set(NRPC_HAS_LIBGPIOD OFF)
if(PLATFORM STREQUAL "linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(NRPC_LIBGPIOD QUIET IMPORTED_TARGET libgpiod)
    endif()
    if(NRPC_LIBGPIOD_FOUND)
        set(NRPC_HAS_LIBGPIOD ON)
        if(NRPC_LIBGPIOD_VERSION VERSION_LESS "2.0")
            target_sources(nrpc PRIVATE src/gpio_libgpiod_v1.cpp)
        else()
            target_sources(nrpc PRIVATE src/gpio_libgpiod_v2.cpp)
        endif()
        target_link_libraries(nrpc PRIVATE PkgConfig::NRPC_LIBGPIOD)
        message(STATUS "GPIO backend enabled with libgpiod ${NRPC_LIBGPIOD_VERSION}")
    else()
        message(STATUS "GPIO backend disabled; libgpiod was not found")
    endif()
endif()

set(NRPC_HAS_TLS ON)
target_sources(nrpc PRIVATE src/tls_transport.cpp)
target_link_libraries(nrpc PRIVATE ${NRPC_MBEDTLS_TARGET})

target_include_directories(nrpc PUBLIC include)
target_link_libraries(nrpc PUBLIC ${NRPC_LIBUV_TARGET})
target_compile_definitions(
    nrpc PUBLIC
    NRPC_HAS_TLS=$<BOOL:${NRPC_HAS_TLS}>
    NRPC_HAS_SERIAL_BACKEND=1
    NRPC_HAS_GPIO_BACKEND=1
    NRPC_HAS_LIBGPIOD=$<BOOL:${NRPC_HAS_LIBGPIOD}>
    NRPC_HAS_NIDAQMX_BACKEND=1
    NRPC_HAS_LCARD_BACKEND=1
)
if(PLATFORM STREQUAL "windows")
    target_link_libraries(nrpc PUBLIC ws2_32)
endif()
nrpc_enable_warnings(nrpc)

add_library(nrpc_server_config src/application_config.cpp)
target_include_directories(nrpc_server_config PUBLIC include)
target_link_libraries(nrpc_server_config PUBLIC nrpc)
nrpc_enable_warnings(nrpc_server_config)

add_executable(nrpc_server src/main.cpp)
if(WIN32)
    target_sources(
        nrpc_server PRIVATE
        resources/nrpc-server.rc
        src/windows_notification_icon.cpp
    )
    target_link_libraries(nrpc_server PRIVATE shell32 user32)
endif()
target_link_libraries(nrpc_server PRIVATE nrpc_server_config)
nrpc_enable_warnings(nrpc_server)
install(TARGETS nrpc_server RUNTIME DESTINATION bin COMPONENT runtime)

include(CTest)
if(BUILD_TESTING)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG "v${NRPC_GOOGLETEST_VERSION}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(googletest)

    if(
        NOT NRPC_PYTHON_CLIENT_SOURCE_DIR
        AND EXISTS "${PROJECT_SOURCE_DIR}/../nrpc-python-client/nrpc/__init__.py"
    )
        set(
            NRPC_PYTHON_CLIENT_SOURCE_DIR
            "${PROJECT_SOURCE_DIR}/../nrpc-python-client"
        )
    endif()
    if(EXISTS "${NRPC_PYTHON_CLIENT_SOURCE_DIR}/nrpc/__init__.py")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(NRPC_HAS_PYTHON_CLIENT ON)
    else()
        set(NRPC_HAS_PYTHON_CLIENT OFF)
        message(STATUS "Python integration tests are disabled; NRPC_PYTHON_CLIENT_SOURCE_DIR was not found")
    endif()

    add_subdirectory(tests)
    install(TARGETS nrpc_server RUNTIME DESTINATION bin COMPONENT tests)
endif()
