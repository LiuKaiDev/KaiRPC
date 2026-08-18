add_executable(rpc_server
    test_rpc_server.cc
    order.pb.cc
)

add_executable(rpc_client
    test_rpc_client.cc
    order.pb.cc
)

add_executable(rpc_service_discovery
    ../net/service_discovery/run_server_discovery.cc
)

target_include_directories(rpc_service_discovery PRIVATE
    ${PROJECT_SOURCE_DIR}/common
)

add_executable(test_interface
    ../net/service_discovery/test_run_time_sd_interface.cc
)

add_executable(tinypb_codec_test
    test_tinypb_coder.cc
)

if(BUILD_TESTING)
    add_executable(tcp_buffer_test
        test_tcp_buffer.cc
    )

    add_executable(service_discovery_integration_test
        test_service_discovery_integration.cc
    )

    add_executable(rpc_loopback_integration_test
        test_rpc_loopback_integration.cc
    )
endif()

target_link_libraries(rpc_server PRIVATE kairpc)
target_link_libraries(rpc_client PRIVATE kairpc)
target_link_libraries(rpc_service_discovery PRIVATE pthread)
target_link_libraries(tinypb_codec_test PRIVATE kairpc)

if(BUILD_TESTING)
    target_link_libraries(tcp_buffer_test PRIVATE kairpc)

    add_test(NAME unit.tcp_buffer COMMAND tcp_buffer_test)
    add_test(NAME unit.tinypb_codec COMMAND tinypb_codec_test)
    add_test(
        NAME integration.service_discovery
        COMMAND service_discovery_integration_test
                $<TARGET_FILE:rpc_service_discovery>
    )
    add_test(
        NAME integration.rpc_loopback
        COMMAND rpc_loopback_integration_test
                $<TARGET_FILE:rpc_service_discovery>
                $<TARGET_FILE:rpc_server>
                $<TARGET_FILE:rpc_client>
    )

    set_tests_properties(
        unit.tcp_buffer
        PROPERTIES
            LABELS "unit"
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(
        unit.tinypb_codec
        PROPERTIES
            LABELS "unit;codec"
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(
        integration.service_discovery
        PROPERTIES
            LABELS "integration;service-discovery"
            RUN_SERIAL TRUE
            TIMEOUT 30
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(
        integration.rpc_loopback
        PROPERTIES
            LABELS "integration;rpc"
            RUN_SERIAL TRUE
            TIMEOUT 30
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
endif()
