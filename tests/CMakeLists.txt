add_executable(nrpc_tests
    arena_test.cpp
    dispatcher_test.cpp
    logger_test.cpp
    protocol_test.cpp
    resource_manager_test.cpp
    smoke_test.cpp
)
if(NRPC_ENABLE_LCARD_BACKEND)
    target_sources(nrpc_tests PRIVATE lcard_backend_test.cpp)
endif()
target_link_libraries(nrpc_tests PRIVATE nrpc GTest::gtest_main)
nrpc_enable_warnings(nrpc_tests)

include(GoogleTest)
gtest_discover_tests(nrpc_tests)

if(NRPC_ENABLE_PYTHON_INTEGRATION_TESTS)
    add_test(
        NAME integration.python_client_server
        COMMAND
            ${CMAKE_COMMAND} -E env
            PYTHONPATH=${NRPC_PYTHON_CLIENT_SOURCE_DIR}
            NRPC_SERVER=$<TARGET_FILE:nrpc_server>
            NRPC_HAS_SERIAL_BACKEND=${NRPC_ENABLE_SERIAL_BACKEND}
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

if(NOT WIN32 AND NRPC_ENABLE_PYTHON_INTEGRATION_TESTS)
    add_executable(
        nrpc_serial_test_server
        ${PROJECT_SOURCE_DIR}/src/main.cpp
        fake_serialport.cpp
    )
    target_link_libraries(nrpc_serial_test_server PRIVATE nrpc)
    nrpc_enable_warnings(nrpc_serial_test_server)
    target_compile_definitions(
        nrpc_serial_test_server PRIVATE $<$<CONFIG:Debug>:DEBUG>
    )

    add_test(
        NAME integration.serial_backend
        COMMAND
            ${CMAKE_COMMAND} -E env
            PYTHONPATH=${NRPC_PYTHON_CLIENT_SOURCE_DIR}
            NRPC_SERIAL_TEST_SERVER=$<TARGET_FILE:nrpc_serial_test_server>
            SERIAL_SCPI_EXAMPLE=${NRPC_PYTHON_CLIENT_SOURCE_DIR}/examples/serial_scpi.py
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/serial_backend_e2e_test.py
    )
    set_tests_properties(integration.serial_backend PROPERTIES TIMEOUT 15)
endif()

if(NOT NRPC_ENABLE_PYTHON_INTEGRATION_TESTS)
    message(
        STATUS
        "Cross-repository Python integration tests are disabled"
    )
endif()
