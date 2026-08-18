cmake_minimum_required(VERSION 3.10)

project(KaiRPC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Protobuf 3.0 REQUIRED)
find_package(Threads REQUIRED)

# KaiRPC uses the classic TinyXML 2.x API (<tinyxml.h>), not TinyXML2.
find_path(TINYXML_INCLUDE_DIR NAMES tinyxml.h)
find_library(TINYXML_LIBRARY NAMES tinyxml)
if(NOT TINYXML_INCLUDE_DIR OR NOT TINYXML_LIBRARY)
    message(FATAL_ERROR
        "KaiRPC requires classic TinyXML 2.x. Install it on Ubuntu/Debian with: "
        "sudo apt-get update && sudo apt-get install -y libtinyxml-dev")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

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

target_include_directories(kairpc
    PUBLIC
        ${PROJECT_SOURCE_DIR}/common
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/net/service_discovery
        ${TINYXML_INCLUDE_DIR}
        ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(kairpc
    PUBLIC
        ${TINYXML_LIBRARY}
        ${Protobuf_LIBRARIES}
        Threads::Threads
)

add_subdirectory(test)
