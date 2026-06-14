cmake_minimum_required(VERSION 3.20)

project(nrpc VERSION 0.1.0 LANGUAGES CXX)

set(
    NRPC_PYTHON_CLIENT_SOURCE_DIR ""
    CACHE PATH "Path to the standalone nrpc-python-client repository"
)
option(
    NRPC_ENABLE_PYTHON_INTEGRATION_TESTS
    "Enable tests that require the standalone Python client repository"
    OFF
)
if(NRPC_ENABLE_PYTHON_INTEGRATION_TESTS)
    if(
        NOT NRPC_PYTHON_CLIENT_SOURCE_DIR
        AND EXISTS "${PROJECT_SOURCE_DIR}/../nrpc-python-client/nrpc/__init__.py"
    )
        set(
            NRPC_PYTHON_CLIENT_SOURCE_DIR
            "${PROJECT_SOURCE_DIR}/../nrpc-python-client"
        )
    endif()
    if(
        NOT EXISTS
        "${NRPC_PYTHON_CLIENT_SOURCE_DIR}/nrpc/__init__.py"
    )
        message(
            FATAL_ERROR
            "NRPC_ENABLE_PYTHON_INTEGRATION_TESTS requires NRPC_PYTHON_CLIENT_SOURCE_DIR"
        )
    endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

find_package(libserialport CONFIG QUIET)
if(TARGET unofficial::libserialport::libserialport)
    set(NRPC_LIBSERIALPORT_TARGET unofficial::libserialport::libserialport)
elseif(TARGET libserialport::libserialport)
    set(NRPC_LIBSERIALPORT_TARGET libserialport::libserialport)
elseif(TARGET serialport)
    set(NRPC_LIBSERIALPORT_TARGET serialport)
else()
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig REQUIRED)
    endif()
    pkg_check_modules(LIBSERIALPORT QUIET IMPORTED_TARGET libserialport)
    if(TARGET PkgConfig::LIBSERIALPORT)
        set(NRPC_LIBSERIALPORT_TARGET PkgConfig::LIBSERIALPORT)
    else()
        find_path(LIBSERIALPORT_INCLUDE_DIR libserialport.h REQUIRED)
        find_library(LIBSERIALPORT_LIBRARY NAMES serialport libserialport REQUIRED)
        add_library(nrpc_libserialport UNKNOWN IMPORTED)
        set_target_properties(
            nrpc_libserialport PROPERTIES
            IMPORTED_LOCATION "${LIBSERIALPORT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBSERIALPORT_INCLUDE_DIR}"
        )
        set(NRPC_LIBSERIALPORT_TARGET nrpc_libserialport)
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
    src/protocol.cpp
    src/resource_manager.cpp
    src/server.cpp
    src/serial_backend.cpp
    src/tcp_backend.cpp
)
target_include_directories(nrpc PUBLIC include)
target_link_libraries(
    nrpc PUBLIC
    ${NRPC_LIBUV_TARGET}
    ${NRPC_LIBSERIALPORT_TARGET}
)
if(WIN32)
    target_link_libraries(nrpc PUBLIC ws2_32)
endif()
nrpc_enable_warnings(nrpc)
target_compile_definitions(nrpc PRIVATE $<$<CONFIG:Debug>:DEBUG>)

add_executable(nrpc_server src/main.cpp)
target_link_libraries(nrpc_server PRIVATE nrpc)
nrpc_enable_warnings(nrpc_server)
target_compile_definitions(nrpc_server PRIVATE $<$<CONFIG:Debug>:DEBUG>)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    if(NRPC_ENABLE_PYTHON_INTEGRATION_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    add_subdirectory(tests)
endif()
