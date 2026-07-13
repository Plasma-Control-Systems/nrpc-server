add_executable(nrpc_tests
    arena_test.cpp
    dispatcher_test.cpp
    logger_test.cpp
    lcard_backend_test.cpp
    nidaqmx_backend_test.cpp
    protocol_test.cpp
    resource_manager_test.cpp
    smoke_test.cpp
)
if(PLATFORM STREQUAL "windows")
    target_sources(nrpc_tests PRIVATE serial_port_win32_test.cpp)
    target_include_directories(nrpc_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()
if(PLATFORM STREQUAL "linux")
    target_sources(nrpc_tests PRIVATE serial_port_posix_test.cpp)
endif()
target_link_libraries(nrpc_tests PRIVATE nrpc GTest::gtest_main)
nrpc_enable_warnings(nrpc_tests)
install(TARGETS nrpc_tests RUNTIME DESTINATION tests COMPONENT tests)

include(GoogleTest)
gtest_discover_tests(nrpc_tests)

if(NRPC_HAS_PYTHON_CLIENT)
    add_test(
        NAME integration.python_client_server
        COMMAND
            ${CMAKE_COMMAND} -E env
            PYTHONPATH=${NRPC_PYTHON_CLIENT_SOURCE_DIR}
            NRPC_SERVER=$<TARGET_FILE:nrpc_server>
            NRPC_HAS_SERIAL_BACKEND=ON
            BASIC_RPC_EXAMPLE=${NRPC_PYTHON_CLIENT_SOURCE_DIR}/examples/basic_rpc.py
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.py
    )
    set_tests_properties(integration.python_client_server PROPERTIES TIMEOUT 15)

    add_test(
        NAME integration.tcp_backend_scpi
        COMMAND
            ${CMAKE_COMMAND} -E env
            PYTHONPATH=${NRPC_PYTHON_CLIENT_SOURCE_DIR}
            NRPC_SERVER=$<TARGET_FILE:nrpc_server>
            MOCK_SCPI=${CMAKE_CURRENT_SOURCE_DIR}/mock_scpi_power_supply.py
            TCP_SCPI_EXAMPLE=${NRPC_PYTHON_CLIENT_SOURCE_DIR}/examples/tcp_scpi.py
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tcp_backend_e2e_test.py
    )
    set_tests_properties(integration.tcp_backend_scpi PROPERTIES TIMEOUT 15)
endif()

if(PLATFORM STREQUAL "linux" AND NRPC_HAS_PYTHON_CLIENT)
    add_test(
        NAME integration.serial_backend
        COMMAND
            ${CMAKE_COMMAND} -E env
            PYTHONPATH=${NRPC_PYTHON_CLIENT_SOURCE_DIR}
            NRPC_SERIAL_TEST_SERVER=$<TARGET_FILE:nrpc_server>
            SERIAL_SCPI_EXAMPLE=${NRPC_PYTHON_CLIENT_SOURCE_DIR}/examples/serial_scpi.py
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/serial_backend_e2e_test.py
    )
    set_tests_properties(integration.serial_backend PROPERTIES TIMEOUT 15)
endif()

if(NOT NRPC_HAS_PYTHON_CLIENT)
    message(
        STATUS
        "Cross-repository Python integration tests are disabled"
    )
endif()
