cmake_minimum_required(VERSION 3.10)
project(NimbusServer)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_AUTOMOC ON)

# 增加 proto 目录和 minio/aws 头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/proto
    /usr/include/mymuduo
    # AWS SDK include path（如有需要可加）
    # /usr/local/include
)

link_directories(
    /usr/lib
    # /usr/local/lib  # 如果aws-sdk-cpp安装在这里
)

# Protobuf
find_package(Protobuf REQUIRED)
message(STATUS "Using protobuf ${Protobuf_VERSION}")

# AWS SDK for C++ (S3)
find_package(AWSSDK REQUIRED COMPONENTS s3)

find_package(Qt5 REQUIRED COMPONENTS Widgets)

# 定义服务器可执行文件及其源文件
add_executable(nimbus_server
    main.cpp
    server/server.cpp
    server/file_server.cpp
    storage/minio_storage.cpp
    ../proto/file.pb.cc
)

# 定义客户端可执行文件
add_executable(file_client
    client/file_client.cpp
    ../proto/file.pb.cc
)

# 定义 Windows 客户端可执行文件
add_executable(file_client_windows
    client/file_client_windows.cpp
    client/draggable_listwidget.cpp
    client/main.cpp
    ../proto/file.pb.cc
)

# 分别设置输出目录
set_target_properties(nimbus_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_server_target
)
set_target_properties(file_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_client_target
)
set_target_properties(file_client_windows PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_client_target
)

target_link_libraries(nimbus_server
    mymuduo
    pthread
    ${Protobuf_LIBRARIES}
    ${AWSSDK_LINK_LIBRARIES}  # AWS S3 SDK
)

target_link_libraries(file_client
    pthread
    ${Protobuf_LIBRARIES}
)

target_link_libraries(file_client_windows
    Qt5::Widgets
    pthread
    ${Protobuf_LIBRARIES}
)