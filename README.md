# Nimbus

Nimbus 是一个基于 **Muduo** 网络库和 **MinIO** 分布式对象存储系统实现的云盘项目，作为毕业设计开发。该系统支持高并发文件上传、下载、管理等功能，具备良好的扩展性和高可用性。

## 技术架构

- **Muduo**：高性能 C++ 网络库，负责实现高并发的 TCP 服务器，处理客户端的连接与数据传输。
- **MinIO**：高性能分布式对象存储系统，兼容 S3 协议，负责后端文件的可靠存储与管理，支持对象存储和高可用性。
- **Protocol Buffers (protobuf)**：作为协议层，负责客户端与服务端之间的数据序列化与通信。
- **Qt**：用于实现跨平台的图形化客户端，提升用户体验。
- **aws-sdk-cpp**：用于与 MinIO 进行 S3 协议通信。
- **MySQL**：用于用户认证和会话管理。

## 主要功能

- 用户文件的上传、下载与删除
- 多用户并发访问支持
- 文件分块存储与断点续传
- MinIO 后端高可用、可扩展的分布式对象存储
- 基于 protobuf 的高效通信协议
- 基于 Qt 的跨平台图形化客户端

## 依赖库

在部署 Nimbus 项目前，请确保已安装以下依赖：

- **mymuduo**：本项目依赖自定义的 Muduo 网络库实现，请先编译并部署 [mymuduo](https://github.com/lionheartyu/mymuduo)。
- **aws-sdk-cpp**：用于与 MinIO 进行 S3 协议通信，请参考 [aws-sdk-cpp 官方文档](https://github.com/aws/aws-sdk-cpp) 进行安装和配置。
- **MinIO**：作为后端对象存储服务，需单独部署，参考 [MinIO 官方文档](https://min.io/)。
- **MySQL**：用于用户认证和会话管理，需安装 MySQL 服务端和开发库（如 `libmysqlclient-dev`）。
- **Protobuf**：用于协议数据序列化，需安装 `protobuf` 和 `protoc`。
- **Qt5**：用于客户端界面，需安装 Qt5 相关开发包（如 `qtbase5-dev`、`qt5-qmake`、`qttools5-dev-tools`）。
- **OpenSSL**：用于加密哈希，需安装 `libssl-dev`。

## 部署与编译

1. **安装依赖库**

   以 Ubuntu 为例，安装常用依赖：
   ```bash
   sudo apt update
   sudo apt install -y build-essential cmake libprotobuf-dev protobuf-compiler \
       libssl-dev libmysqlclient-dev qtbase5-dev qt5-qmake qttools5-dev-tools
   # aws-sdk-cpp 和 mymuduo 需参考各自文档编译安装
   ```

2. **编译 mymuduo**
   ```bash
   git clone https://github.com/lionheartyu/mymuduo.git
   cd mymuduo
   mkdir build && cd build
   cmake ..
   make -j
   sudo make install
   ```

3. **编译 aws-sdk-cpp**
   ```bash
   git clone https://github.com/aws/aws-sdk-cpp.git
   cd aws-sdk-cpp
   mkdir build && cd build
   cmake .. -DBUILD_ONLY="s3"
   make -j
   sudo make install
   ```

4. **部署 MinIO 和 MySQL**
   - MinIO: 参考 [MinIO 官方文档](https://min.io/docs/minio/linux/index.html) 部署并创建桶。
   - MySQL: 安装并初始化数据库，导入表结构（见 `server/file_server_db.cpp` 相关注释）。

5. **编译 Nimbus 项目**
   ```bash
   cd /path/to/Nimbus/src
   mkdir build && cd build
   cmake ..
   make -j
   ```

6. **运行服务端**
   ```bash
   ./n_server_target/nimbus_server
   ```

7. **运行客户端**
   ```bash
   ./n_client_target/file_client_windows
   ```

## 其他说明

- 配置文件和数据库参数可在源码中修改（如 `file_server.cpp`）。
- 若遇到依赖缺失或链接错误，请检查 CMakeLists.txt 的 include 和 link 路径设置。
- 推荐使用 C++17 及以上标准进行编译。

---

如有问题请参考各依赖库官方文档或提 issue。