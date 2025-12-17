cmake_minimum_required(VERSION 3.10)
project(NimbusServer)

set(CMAKE_CXX_STANDARD 14)

# 增加 proto 目录和 ceph 头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/proto
    /usr/include/mymuduo
    /usr/include/rados
)

link_directories(
    /usr/lib
)

# Protobuf
find_package(Protobuf REQUIRED)
message(STATUS "Using protobuf ${Protobuf_VERSION}")

# 定义服务器可执行文件及其源文件
add_executable(nimbus_server
    main.cpp
    server/server.cpp
    server/file_server.cpp
    storage/ceph_storage.cpp
    ../proto/file.pb.cc
)

# 定义客户端可执行文件
add_executable(file_client
    client/file_client.cpp
    ../proto/file.pb.cc
)

# 分别设置输出目录
set_target_properties(nimbus_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_server_target
)
set_target_properties(file_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_client_target
)

target_link_libraries(nimbus_server
    mymuduo
    pthread
    rados
    ${Protobuf_LIBRARIES}
)

target_link_libraries(file_client
    pthread
    ${Protobuf_LIBRARIES}
)