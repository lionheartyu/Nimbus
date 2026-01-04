cmake_minimum_required(VERSION 3.10)
project(NimbusServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

# ========== 头文件搜索路径 ==========
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/proto
    /usr/include/mymuduo
)

# ========== 库文件搜索路径 ==========
link_directories(
    /usr/lib
)

# ========== 查找 Protobuf ==========
find_package(Protobuf REQUIRED)
message(STATUS "Using protobuf ${Protobuf_VERSION}")

# ========== 查找 AWS S3 SDK ==========
find_package(AWSSDK REQUIRED COMPONENTS s3)

# ========== 查找 Qt5 组件 ==========
find_package(Qt5 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Concurrent)

# ========== 查找 OpenSSL ==========
find_package(OpenSSL REQUIRED)

# ========== 查找 MySQL C API ==========
# Ubuntu/Debian 的 libmysqlclient-dev 通常不带 CMake config，用 Find 兜底
find_path(MYSQL_INCLUDE_DIR
    NAMES mysql/mysql.h
    PATHS /usr/include /usr/local/include
)

find_library(MYSQL_LIBRARY
    NAMES mysqlclient
    PATHS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR "MySQL client library not found. Install: sudo apt install -y libmysqlclient-dev")
endif()

message(STATUS "MySQL include: ${MYSQL_INCLUDE_DIR}")
message(STATUS "MySQL library: ${MYSQL_LIBRARY}")

include_directories(${MYSQL_INCLUDE_DIR})

# ========== 服务器可执行文件及其源文件 ==========
add_executable(nimbus_server
    main.cpp
    server/file_server.cpp
    server/file_server_util.cpp
    server/file_server_chunk.cpp
    server/file_server_db.cpp
    storage/minio_storage.cpp
    ../proto/file.pb.cc
)

# ========== Windows 客户端可执行文件（Qt） ==========
add_executable(file_client_windows
    client/file_client_windows.cpp
    client/draggable_listwidget.cpp
    client/login_dialog.cpp
    client/register_dialog.cpp
    client/main.cpp
    ../proto/file.pb.cc
)

# ========== 设置输出目录 ==========
set_target_properties(nimbus_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_server_target
)

set_target_properties(file_client_windows PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/n_client_target
)

# ========== 链接服务器依赖库 ==========
target_link_libraries(nimbus_server
    mymuduo
    pthread
    ${Protobuf_LIBRARIES}
    ${AWSSDK_LINK_LIBRARIES}
    ${MYSQL_LIBRARY}
    OpenSSL::SSL
    OpenSSL::Crypto
)

# ========== 链接客户端依赖库 ==========
target_link_libraries(file_client_windows
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::MultimediaWidgets
    Qt5::Concurrent
    pthread
    ${Protobuf_LIBRARIES}
)