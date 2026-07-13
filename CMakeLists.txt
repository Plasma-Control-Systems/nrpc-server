cmake_minimum_required(VERSION 3.20)

project(nrpc VERSION 0.1.0 LANGUAGES CXX)

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
set(
    NRPC_THIRDPARTY_DIR
    "${PROJECT_SOURCE_DIR}/thirdparty"
    CACHE PATH "Windows-only directory containing include/ and libs/ folders"
)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PLATFORM STREQUAL "windows")
    set(NRPC_THIRDPARTY_INCLUDE_DIR "${NRPC_THIRDPARTY_DIR}/include")
    set(NRPC_THIRDPARTY_LIBRARY_DIR "${NRPC_THIRDPARTY_DIR}/libs")

    find_path(
        NRPC_LIBUV_INCLUDE_DIR
        NAMES uv.h
        PATHS "${NRPC_THIRDPARTY_INCLUDE_DIR}"
        NO_DEFAULT_PATH
    )
    find_library(
        NRPC_LIBUV_LIBRARY
        NAMES uv_a libuv_a libuv uv
        PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    if(NOT NRPC_LIBUV_INCLUDE_DIR OR NOT NRPC_LIBUV_LIBRARY)
        message(
            FATAL_ERROR
            "Windows builds require libuv in ${NRPC_THIRDPARTY_DIR}: "
            "put uv.h under include/ and uv_a.lib, libuv_a.lib, "
            "libuv.lib, or uv.lib under libs/"
        )
    endif()

    add_library(nrpc_libuv UNKNOWN IMPORTED)
    set_target_properties(
        nrpc_libuv PROPERTIES
        IMPORTED_LOCATION "${NRPC_LIBUV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NRPC_LIBUV_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "ws2_32;iphlpapi;psapi;userenv;dbghelp"
    )
    set(NRPC_LIBUV_TARGET nrpc_libuv)
else()
    find_package(libuv CONFIG QUIET)
    if(TARGET unofficial::libuv::libuv)
        set(NRPC_LIBUV_TARGET unofficial::libuv::libuv)
    elseif(TARGET libuv::uv)
        set(NRPC_LIBUV_TARGET libuv::uv)
    elseif(TARGET uv)
        set(NRPC_LIBUV_TARGET uv)
    elseif(TARGET uv_a)
        set(NRPC_LIBUV_TARGET uv_a)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
        set(NRPC_LIBUV_TARGET PkgConfig::LIBUV)
    endif()
endif()

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
    src/logger.cpp
    src/nidaqmx_backend.cpp
    src/protocol.cpp
    src/resource_manager.cpp
    src/serial_backend.cpp
    src/server.cpp
    src/tcp_backend.cpp
)
if(PLATFORM STREQUAL "windows")
    target_sources(nrpc PRIVATE src/serial_port_win32.cpp)
else()
    target_sources(nrpc PRIVATE src/serial_port_posix.cpp)
endif()

set(NRPC_HAS_TLS OFF)
if(PLATFORM STREQUAL "windows")
    find_path(
        NRPC_MBEDTLS_INCLUDE_DIR
        NAMES mbedtls/ssl.h
        PATHS "${NRPC_THIRDPARTY_INCLUDE_DIR}"
        NO_DEFAULT_PATH
    )
    find_library(
        NRPC_MBEDTLS_LIBRARY
        NAMES mbedtls
        PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    find_library(
        NRPC_MBEDX509_LIBRARY
        NAMES mbedx509
        PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    find_library(
        NRPC_MBEDCRYPTO_LIBRARY
        NAMES mbedcrypto
        PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}"
        NO_DEFAULT_PATH
    )
    if(
        NRPC_MBEDTLS_INCLUDE_DIR
        AND NRPC_MBEDTLS_LIBRARY
        AND NRPC_MBEDX509_LIBRARY
        AND NRPC_MBEDCRYPTO_LIBRARY
    )
        add_library(nrpc_mbedtls INTERFACE)
        target_include_directories(nrpc_mbedtls INTERFACE "${NRPC_MBEDTLS_INCLUDE_DIR}")
        target_link_libraries(
            nrpc_mbedtls INTERFACE
            "${NRPC_MBEDTLS_LIBRARY}"
            "${NRPC_MBEDX509_LIBRARY}"
            "${NRPC_MBEDCRYPTO_LIBRARY}"
            bcrypt
        )
        set(NRPC_MBEDTLS_TARGET nrpc_mbedtls)
        set(NRPC_HAS_TLS ON)
    else()
        message(STATUS "mbedTLS not found in ${NRPC_THIRDPARTY_DIR}; TLS is unavailable")
    endif()
else()
    find_package(MbedTLS QUIET)
    if(TARGET MbedTLS::mbedtls)
        set(NRPC_MBEDTLS_TARGET MbedTLS::mbedtls)
        set(NRPC_HAS_TLS ON)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(
                MBEDTLS QUIET IMPORTED_TARGET
                mbedtls mbedx509 mbedcrypto
            )
        endif()
        if(TARGET PkgConfig::MBEDTLS)
            set(NRPC_MBEDTLS_TARGET PkgConfig::MBEDTLS)
            set(NRPC_HAS_TLS ON)
        else()
            message(STATUS "mbedTLS not found; TLS is unavailable")
        endif()
    endif()
endif()
if(NRPC_HAS_TLS)
    target_sources(nrpc PRIVATE src/tls_transport.cpp)
    target_link_libraries(nrpc PRIVATE ${NRPC_MBEDTLS_TARGET})
endif()

target_include_directories(nrpc PUBLIC include)
target_link_libraries(nrpc PUBLIC ${NRPC_LIBUV_TARGET})
target_compile_definitions(
    nrpc PUBLIC
    NRPC_HAS_TLS=$<BOOL:${NRPC_HAS_TLS}>
    NRPC_HAS_SERIAL_BACKEND=1
    NRPC_HAS_NIDAQMX_BACKEND=1
)
if(PLATFORM STREQUAL "windows")
    target_link_libraries(nrpc PUBLIC ws2_32)
endif()
nrpc_enable_warnings(nrpc)

add_executable(nrpc_server src/main.cpp)
target_link_libraries(nrpc_server PRIVATE nrpc)
nrpc_enable_warnings(nrpc_server)
install(TARGETS nrpc_server RUNTIME DESTINATION bin COMPONENT runtime)

include(CTest)
if(BUILD_TESTING)
    if(PLATFORM STREQUAL "windows")
        find_package(
            GTest CONFIG QUIET
            PATHS "${NRPC_THIRDPARTY_DIR}/libs/cmake/GTest"
        )
    else()
        find_package(GTest CONFIG QUIET)
    endif()
    if(NOT TARGET GTest::gtest_main)
        include(FetchContent)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        FetchContent_MakeAvailable(googletest)
    endif()

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
