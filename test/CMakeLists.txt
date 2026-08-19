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

    add_executable(tcp_write_path_test
        test_tcp_write_path.cc
    )

    add_executable(fd_epoll_test
        test_fd_epoll.cc
    )

    add_executable(lifetime_iothread_test
        test_lifetime_iothread.cc
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
    target_link_libraries(tcp_write_path_test PRIVATE kairpc)
    target_link_libraries(fd_epoll_test PRIVATE kairpc)
    target_link_libraries(lifetime_iothread_test PRIVATE kairpc)

    add_test(NAME unit.tcp_buffer COMMAND tcp_buffer_test)
    add_test(NAME unit.tinypb_codec COMMAND tinypb_codec_test)
    add_test(NAME unit.tcp_write.complete COMMAND tcp_write_path_test complete)
    add_test(NAME unit.tcp_write.partial COMMAND tcp_write_path_test partial)
    add_test(NAME unit.tcp_write.eagain COMMAND tcp_write_path_test eagain)
    add_test(NAME unit.fd_lifecycle.close COMMAND fd_epoll_test close)
    add_test(NAME unit.fd_lifecycle.idempotent COMMAND fd_epoll_test idempotent)
    add_test(NAME unit.fd_lifecycle.repeat COMMAND fd_epoll_test repeat)
    add_test(NAME unit.epoll.error_dispatch COMMAND fd_epoll_test error_dispatch)
    add_test(NAME unit.epoll.hup COMMAND fd_epoll_test hup)
    add_test(NAME unit.epoll.rdhup COMMAND fd_epoll_test rdhup)
    add_test(NAME unit.epoll.rdhup_with_data COMMAND fd_epoll_test rdhup_with_data)
    add_test(NAME unit.iothread.start_stop COMMAND lifetime_iothread_test start_stop)
    add_test(NAME unit.iothread.destructor COMMAND lifetime_iothread_test destructor)
    add_test(NAME unit.iothread.idempotent_stop COMMAND lifetime_iothread_test idempotent_stop)
    add_test(NAME unit.iothreadgroup.shutdown COMMAND lifetime_iothread_test group_shutdown)
    add_test(NAME unit.iothreadgroup.repeat COMMAND lifetime_iothread_test repeat)
    add_test(NAME unit.lifetime.deferred_callback COMMAND lifetime_iothread_test deferred_callback)
    add_test(NAME unit.lifetime.timer_cancel COMMAND lifetime_iothread_test timer_cancel)
    add_test(NAME unit.shutdown.server COMMAND lifetime_iothread_test server_shutdown)
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
        unit.tcp_write.complete
        unit.tcp_write.partial
        unit.tcp_write.eagain
        PROPERTIES
            LABELS "unit;tcp-write"
            TIMEOUT 15
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(
        unit.fd_lifecycle.close
        unit.fd_lifecycle.idempotent
        unit.fd_lifecycle.repeat
        unit.epoll.error_dispatch
        unit.epoll.hup
        unit.epoll.rdhup
        unit.epoll.rdhup_with_data
        PROPERTIES
            LABELS "unit;network;fd-lifecycle;epoll;tcp"
            TIMEOUT 15
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    )
    set_tests_properties(
        unit.iothread.start_stop
        unit.iothread.destructor
        unit.iothread.idempotent_stop
        unit.iothreadgroup.shutdown
        unit.iothreadgroup.repeat
        unit.lifetime.deferred_callback
        unit.lifetime.timer_cancel
        unit.shutdown.server
        PROPERTIES
            LABELS "unit;network;lifetime;iothread;shutdown"
            TIMEOUT 15
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
