#include "file_server.h"
#include <cstring>
#include <iostream>
#include <iomanip>

FileServer::FileServer(EventLoop* loop, const InetAddress& addr, const std::string& name)
    : server_(loop, addr, name), loop_(loop) {
    // 设置连接建立/断开时的回调
    server_.setConnectionCallback(std::bind(&FileServer::onConnection, this, std::placeholders::_1));
    // 设置消息到达时的回调
    server_.setMessageCallback(std::bind(&FileServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    // 设置工作线程数量
    server_.setThreadNum(3);

    // 初始化 MinioStorage（参数请根据你的实际配置填写）
    minio_ = std::make_unique<MinioStorage>(
        "127.0.0.1:9000",   // MinIO endpoint
        "minioadmin",       // Access Key
        "minioadmin",       // Secret Key
        "data"              // Bucket name
    );
}

void FileServer::start() {
    server_.start();
}

// 连接建立或断开时的回调函数
void FileServer::onConnection(const TcpConnectionPtr& conn) {
    if (!conn->connected()) {
        // 连接断开，清理状态
        if (outfile_.is_open()) outfile_.close();
        receiving_ = false;
        file_size_ = 0;
        received_ = 0;
        filename_.clear();
        pb_head_parsed_ = false;
        pb_head_len_ = 0;
        pb_head_buf_.clear();
        std::cout << "Connection closed." << std::endl;
    } else {
        // 新连接建立
        std::cout << "New connection from " << conn->peerAddress().toIpPort() << std::endl;
    }
}

// 消息到达时的回调函数
void FileServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
    while (buf->readableBytes() > 0) {
        if (!pb_head_parsed_) {
            // 先收4字节长度
            if (pb_head_len_ == 0) {
                if (buf->readableBytes() < 4) return;
                memcpy(&pb_head_len_, buf->peek(), 4);
                buf->retrieve(4);
            }
            // 再收protobuf头
            if (buf->readableBytes() < pb_head_len_) return;
            pb_head_buf_.assign(buf->peek(), pb_head_len_);
            buf->retrieve(pb_head_len_);

            // 反序列化
            FileHeader header;
            if (!header.ParseFromString(pb_head_buf_)) {
                conn->send("ERROR: Protobuf parse failed\n");
                conn->shutdown();
                return;
            }
            filename_ = header.filename();
            file_size_ = header.filesize();
            pb_head_parsed_ = true;

            // 打开输出文件
            outfile_.open(filename_, std::ios::binary);
            if (!outfile_) {
                conn->send("ERROR: Cannot open file\n");
                conn->shutdown();
                return;
            }
            receiving_ = true;
            received_ = 0;
            std::cout << "Start receiving file: " << filename_ << ", size: " << file_size_ << std::endl;

            // === 处理空文件 ===
            if (file_size_ == 0) {
                outfile_.close();
                receiving_ = false;
                conn->send("UPLOAD OK\n");
                std::cout << std::endl << "File received: " << filename_ << std::endl;

                // 上传到 Minio
                if (minio_ && minio_->upload(filename_, filename_)) {
                    std::cout << "Upload to Minio success: " << filename_ << std::endl;
                    std::remove(filename_.c_str());
                } else {
                    std::cerr << "Upload to Minio failed: " << filename_ << std::endl;
                }
                // 重置状态，准备下一个文件
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }
        } else if (receiving_) {
            // 正在接收文件内容
            size_t to_write = std::min(static_cast<uint64_t>(buf->readableBytes()), file_size_ - received_);
            outfile_.write(buf->peek(), to_write); // 写入文件
            buf->retrieve(to_write);               // 移除已处理数据
            received_ += to_write;

            // 显示进度条
            double percent = 100.0 * received_ / file_size_;
            std::cout << "\r接收进度: " << received_ << "/" << file_size_
                      << " 字节 (" << std::fixed << std::setprecision(2) << percent << "%)" << std::flush;

            if (received_ >= file_size_) {
                // 文件接收完毕
                outfile_.close();
                receiving_ = false;
                conn->send("UPLOAD OK\n");
                std::cout << std::endl << "File received: " << filename_ << std::endl;

                // 上传到 Minio
                if (minio_ && minio_->upload(filename_, filename_)) {
                    std::cout << "Upload to Minio success: " << filename_ << std::endl;
                    // 可选：删除本地临时文件
                    std::remove(filename_.c_str());
                } else {
                    std::cerr << "Upload to Minio failed: " << filename_ << std::endl;
                }
            }
        } else {
            // 已经收到全部数据，忽略后续数据
            buf->retrieveAll();
        }
    }
}

