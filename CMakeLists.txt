cmake_minimum_required(VERSION 3.10)

project(KaiRPC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include_directories(${PROJECT_SOURCE_DIR}/common)
include_directories(${PROJECT_SOURCE_DIR}/net)
include_directories(${PROJECT_SOURCE_DIR}/net/service_discovery)

aux_source_directory(${PROJECT_SOURCE_DIR}/common COMMON_LIST)
aux_source_directory(${PROJECT_SOURCE_DIR}/net NET_LIST)
aux_source_directory(${PROJECT_SOURCE_DIR}/net/tcp TCP_LIST)
aux_source_directory(${PROJECT_SOURCE_DIR}/net/coder CODER_LIST)
aux_source_directory(${PROJECT_SOURCE_DIR}/net/rpc RPC_LIST)

add_library(kairpc STATIC
    ${COMMON_LIST}
    ${NET_LIST}
    ${TCP_LIST}
    ${CODER_LIST}
    ${RPC_LIST}
)

target_link_libraries(kairpc PUBLIC tinyxml pthread protobuf)

add_subdirectory(test)
