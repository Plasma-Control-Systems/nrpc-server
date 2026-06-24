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
option(
    NRPC_ENABLE_SERIAL_BACKEND
    "Build the serial backend"
    ON
)
option(
    NRPC_ENABLE_LCARD_BACKEND
    "Build the Windows LCard backend when the vendor library is available"
    ON
)
option(
    NRPC_ENABLE_NIDAQMX_BACKEND
    "Build the Windows NI-DAQmx backend"
    ON
)
option(
    NRPC_ENABLE_TLS
    "Build optional TLS server support using mbedTLS"
    ON
)
option(
    NRPC_FETCH_GTEST
    "Download GoogleTest with FetchContent when tests are enabled and no package is installed"
    ON
)
set(
    NRPC_THIRDPARTY_DIR
    "${PROJECT_SOURCE_DIR}/thirdparty"
    CACHE PATH "Windows-only directory containing include/ and libs/ folders"
)
set(
    NRPC_LCARD_LIBRARY_DIR
    "C:/Program Files (x86)/LCard/LIBRARY/BIN"
    CACHE PATH "Path to the LCard lcomp/wlcomp runtime DLL directory"
)
set(
    NRPC_LCARD_BIOS_DIR
    "C:/Program Files (x86)/LCard/LIBRARY/BIOS"
    CACHE PATH "Path to the default LCard BIOS and PLD directory"
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

if(WIN32)
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

if(NRPC_ENABLE_LCARD_BACKEND)
    if(
        NOT WIN32
    )
        message(
            STATUS
            "LCard lcomp/wlcomp is Windows-only; building without the LCard backend"
        )
        set(NRPC_ENABLE_LCARD_BACKEND OFF)
    endif()
endif()
if(NRPC_ENABLE_NIDAQMX_BACKEND AND NOT WIN32)
    message(STATUS "NI-DAQmx is Windows-only; building without the NI-DAQmx backend")
    set(NRPC_ENABLE_NIDAQMX_BACKEND OFF)
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
    src/tcp_backend.cpp
)
if(NRPC_ENABLE_TLS)
    if(WIN32)
        find_path(NRPC_MBEDTLS_INCLUDE_DIR NAMES mbedtls/ssl.h PATHS "${NRPC_THIRDPARTY_INCLUDE_DIR}" NO_DEFAULT_PATH)
        find_library(NRPC_MBEDTLS_LIBRARY NAMES mbedtls PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}" NO_DEFAULT_PATH)
        find_library(NRPC_MBEDX509_LIBRARY NAMES mbedx509 PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}" NO_DEFAULT_PATH)
        find_library(NRPC_MBEDCRYPTO_LIBRARY NAMES mbedcrypto PATHS "${NRPC_THIRDPARTY_LIBRARY_DIR}" NO_DEFAULT_PATH)
        if(NRPC_MBEDTLS_INCLUDE_DIR AND NRPC_MBEDTLS_LIBRARY AND NRPC_MBEDX509_LIBRARY AND NRPC_MBEDCRYPTO_LIBRARY)
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
        else()
            message(STATUS "mbedTLS not found in ${NRPC_THIRDPARTY_DIR}; building without TLS")
            set(NRPC_ENABLE_TLS OFF)
        endif()
    else()
        find_package(MbedTLS QUIET)
        if(TARGET MbedTLS::mbedtls)
            set(NRPC_MBEDTLS_TARGET MbedTLS::mbedtls)
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
            else()
                message(STATUS "mbedTLS not found; building without TLS")
                set(NRPC_ENABLE_TLS OFF)
            endif()
        endif()
    endif()
endif()
if(NRPC_ENABLE_TLS)
    target_sources(nrpc PRIVATE src/tls_transport.cpp)
    target_link_libraries(nrpc PRIVATE ${NRPC_MBEDTLS_TARGET})
endif()
if(NRPC_ENABLE_SERIAL_BACKEND)
    target_sources(nrpc PRIVATE src/serial_backend.cpp)
    if(WIN32)
        target_sources(nrpc PRIVATE src/serial_port_win32.cpp)
    else()
        target_sources(nrpc PRIVATE src/serial_port_posix.cpp)
    endif()
endif()
if(NRPC_ENABLE_LCARD_BACKEND)
    target_sources(nrpc PRIVATE src/lcard_backend.cpp)
endif()
if(NRPC_ENABLE_NIDAQMX_BACKEND)
    target_sources(nrpc PRIVATE src/nidaqmx_backend.cpp)
endif()
target_include_directories(nrpc PUBLIC include)
target_link_libraries(nrpc PUBLIC ${NRPC_LIBUV_TARGET})
target_compile_definitions(nrpc PUBLIC NRPC_HAS_TLS=$<BOOL:${NRPC_ENABLE_TLS}>)
if(NRPC_ENABLE_SERIAL_BACKEND)
    target_compile_definitions(nrpc PUBLIC NRPC_HAS_SERIAL_BACKEND=1)
else()
    target_compile_definitions(nrpc PUBLIC NRPC_HAS_SERIAL_BACKEND=0)
endif()
if(NRPC_ENABLE_LCARD_BACKEND)
    target_compile_definitions(
        nrpc PUBLIC NRPC_HAS_LCARD_BACKEND=1
        PRIVATE
            NRPC_LCARD_LIBRARY_DIR="${NRPC_LCARD_LIBRARY_DIR}"
            NRPC_LCARD_BIOS_DIR="${NRPC_LCARD_BIOS_DIR}"
    )
else()
    target_compile_definitions(nrpc PUBLIC NRPC_HAS_LCARD_BACKEND=0)
endif()
if(NRPC_ENABLE_NIDAQMX_BACKEND)
    target_compile_definitions(nrpc PUBLIC NRPC_HAS_NIDAQMX_BACKEND=1)
else()
    target_compile_definitions(nrpc PUBLIC NRPC_HAS_NIDAQMX_BACKEND=0)
endif()
if(WIN32)
    target_link_libraries(nrpc PUBLIC ws2_32)
endif()
nrpc_enable_warnings(nrpc)
target_compile_definitions(nrpc PRIVATE $<$<CONFIG:Debug>:DEBUG>)

add_executable(nrpc_server src/main.cpp)
target_link_libraries(nrpc_server PRIVATE nrpc)
nrpc_enable_warnings(nrpc_server)
target_compile_definitions(nrpc_server PRIVATE $<$<CONFIG:Debug>:DEBUG>)
install(TARGETS nrpc_server RUNTIME DESTINATION bin COMPONENT runtime)

include(CTest)
if(BUILD_TESTING)
    if(WIN32)
        find_package(
            GTest CONFIG QUIET
            PATHS "${NRPC_THIRDPARTY_DIR}/libs/cmake/GTest"
        )
    else()
        find_package(GTest CONFIG QUIET)
    endif()
    if(NOT TARGET GTest::gtest_main)
        if(NRPC_FETCH_GTEST)
            include(FetchContent)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            FetchContent_Declare(
                googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG v1.14.0
            )
            FetchContent_MakeAvailable(googletest)
        else()
            message(
                FATAL_ERROR
                "GTest was not found. Install GoogleTest or configure with "
                "-DNRPC_FETCH_GTEST=ON to download it for test builds."
            )
        endif()
    endif()
    if(NRPC_ENABLE_PYTHON_INTEGRATION_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    if(BUILD_TESTING)
        add_subdirectory(tests)
        install(TARGETS nrpc_server RUNTIME DESTINATION bin COMPONENT tests)
    endif()
endif()
