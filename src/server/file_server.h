#pragma once
#include <mymuduo/TcpServer.h>
#include <string>
#include <fstream>
#include <memory>
#include "../storage/minio_storage.h"
#include "../../proto/file.pb.h" // 路径按实际调整

/// 文件服务器主类，负责处理客户端连接、文件上传下载、分片合并等
class FileServer
{
public:
    /// 构造函数
    FileServer(EventLoop *loop, const InetAddress &addr, const std::string &name);

    /// 启动服务器
    void start();

private:
    /// 连接事件回调
    void onConnection(const TcpConnectionPtr &conn);

    /// 消息事件回调（主协议处理）
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time);

    // =========================
    // 文件接收状态
    // =========================
    bool receiving_ = false;           ///< 是否正在接收文件内容
    std::ofstream outfile_;            ///< 当前写入的文件流
    uint64_t file_size_ = 0;           ///< 当前文件总大小
    uint64_t received_ = 0;            ///< 已接收字节数
    std::string filename_;             ///< 当前文件名

    // =========================
    // Protobuf头部解析状态
    // =========================
    bool pb_head_parsed_ = false;      ///< 是否已解析protobuf头
    uint32_t pb_head_len_ = 0;         ///< protobuf头长度
    std::string pb_head_buf_;          ///< protobuf头内容

    EventLoop *loop_;                  ///< 事件循环指针
    TcpServer server_;                 ///< TCP服务器对象

    // Minio存储成员，支持上传、下载、列举等操作
    std::unique_ptr<MinioStorage> minio_;
};