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

add_executable(test_interface
    ../net/service_discovery/test_run_time_sd_interface.cc
)

add_executable(tinypb_codec_test
    test_tinypb_coder.cc
)

target_link_libraries(rpc_server PRIVATE kairpc)
target_link_libraries(rpc_client PRIVATE kairpc)
target_link_libraries(rpc_service_discovery PRIVATE pthread)
target_link_libraries(tinypb_codec_test PRIVATE kairpc)
