#pragma once
#include <mymuduo/TcpServer.h>
#include <string>
#include <fstream>
#include <memory>
#include "../storage/minio_storage.h"
#include "../../proto/file.pb.h" // 路径按实际调整

class FileServer
{
public:
    FileServer(EventLoop *loop, const InetAddress &addr, const std::string &name);
    void start();

private:
    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time);

    // 文件接收状态
    bool receiving_ = false;
    std::ofstream outfile_;
    uint64_t file_size_ = 0;
    uint64_t received_ = 0;
    std::string filename_;

    // Protobuf头部解析
    bool pb_head_parsed_ = false;
    uint32_t pb_head_len_ = 0;
    std::string pb_head_buf_;

    EventLoop *loop_;
    TcpServer server_;
    
    // Minio存储成员，支持上传、下载、列举
    std::unique_ptr<MinioStorage> minio_;
};