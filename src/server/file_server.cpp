#include "file_server.h"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>

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
                conn->send(std::string("ERROR: Protobuf parse failed\n"));
                conn->shutdown();
                return;
            }
            filename_ = header.filename();
            file_size_ = header.filesize();
            pb_head_parsed_ = true;

            // ====== 列举文件 ======
            if (header.type() == 3) { // type=3 表示列举
                std::vector<std::string> files;
                if (minio_ && minio_->listObjects(files)) {
                    ListFilesResponse resp;
                    for (const auto& f : files) resp.add_filenames(f);
                    std::string out;
                    resp.SerializeToString(&out);

                    uint32_t len = static_cast<uint32_t>(out.size());
                    conn->send(std::string(reinterpret_cast<const char*>(&len), 4));
                    conn->send(out);
                } else {
                    conn->send(std::string("ERROR: List files failed\n"));
                }
                // 重置状态
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }

            // ====== 下载文件 ======
            if (header.type() == 2) { // type=2 表示下载
                std::string tmp_path = "/tmp/" + filename_;
                bool ok = minio_ && minio_->download(filename_, tmp_path);
                if (!ok) {
                    conn->send(std::string("ERROR: Download from Minio failed\n"));
                    conn->shutdown();
                    return;
                }
                std::ifstream infile(tmp_path, std::ios::binary | std::ios::ate);
                if (!infile) {
                    conn->send(std::string("ERROR: Cannot open downloaded file\n"));
                    conn->shutdown();
                    return;
                }
                std::streamsize filesize = infile.tellg();
                infile.seekg(0, std::ios::beg);

                // 发送4字节文件长度
                uint32_t len = static_cast<uint32_t>(filesize);
                conn->send(std::string(reinterpret_cast<const char*>(&len), 4));

                // 分块发送文件内容
                char buf4k[4096];
                while (infile) {
                    infile.read(buf4k, sizeof(buf4k));
                    std::streamsize n = infile.gcount();
                    if (n > 0) conn->send(std::string(buf4k, static_cast<size_t>(n)));
                }
                infile.close();
                std::remove(tmp_path.c_str());
                conn->shutdown();
                // 重置状态
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }

            // ====== 删除（移入回收站） ======
            if (header.type() == 4) { // type=4 表示删除到回收站
                std::string recycle_name = std::string("recycle/") + filename_;
                bool ok = false;
                if (minio_) {
                    // 先拷贝到 recycle/ 前缀，再删除原对象
                    ok = minio_->copyObject(filename_, recycle_name) && minio_->remove(filename_);
                }
                if (ok) {
                    conn->send(std::string("DELETE OK\n"));
                } else {
                    conn->send(std::string("ERROR: Delete to recycle failed\n"));
                }
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }

            // ====== 列举回收站 ======
            if (header.type() == 5) { // type=5 表示列举回收站
                std::vector<std::string> files;
                if (minio_ && minio_->listObjectsWithPrefix("recycle/", files)) {
                    ListFilesResponse resp;
                    for (const auto& f : files) resp.add_filenames(f);
                    std::string out;
                    resp.SerializeToString(&out);

                    uint32_t len = static_cast<uint32_t>(out.size());
                    conn->send(std::string(reinterpret_cast<const char*>(&len), 4));
                    conn->send(out);
                } else {
                    conn->send(std::string("ERROR: List recycle failed\n"));
                }
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }

            // ====== 还原回收站文件 ======
            if (header.type() == 6) { // type=6 表示还原
                // 如果 extra 字段提供原始路径则使用，否则把文件名直接还原到根目录
                std::string origin_name = header.extra().empty() ? filename_ : header.extra();
                std::string recycle_name = std::string("recycle/") + filename_;
                bool ok = false;
                if (minio_) {
                    ok = minio_->copyObject(recycle_name, origin_name) && minio_->remove(recycle_name);
                }
                if (ok) {
                    conn->send(std::string("RESTORE OK\n"));
                } else {
                    conn->send(std::string("ERROR: Restore failed\n"));
                }
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }

            // ====== 彻底删除回收站文件 ======
            if (header.type() == 7) { // type=7 表示彻底删除
                std::string recycle_name = std::string("recycle/") + filename_;
                bool ok = minio_ && minio_->remove(recycle_name);
                if (ok) {
                    conn->send(std::string("REMOVE OK\n"));
                } else {
                    conn->send(std::string("ERROR: Remove failed\n"));
                }
                pb_head_parsed_ = false;
                pb_head_len_ = 0;
                pb_head_buf_.clear();
                filename_.clear();
                file_size_ = 0;
                received_ = 0;
                return;
            }

            // ====== 上传文件（原有逻辑） ======
            outfile_.open(filename_, std::ios::binary);
            if (!outfile_) {
                conn->send(std::string("ERROR: Cannot open file\n"));
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
                conn->send(std::string("UPLOAD OK\n"));
                std::cout << std::endl << "File received: " << filename_ << std::endl;

                // 上传到 Minio
                if (minio_ && minio_->upload(filename_, filename_)) {
                    std::cout << "Upload to Minio success: " << filename_ << std::endl;
                    std::remove(filename_.c_str());
                } else {
                    std::cerr << "Upload to Minio failed: " << filename_ << std::endl;
                }
                // 重置状态
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
            outfile_.write(buf->peek(), to_write);
            buf->retrieve(to_write);
            received_ += to_write;

            double percent = 100.0 * received_ / file_size_;
            std::cout << "\r接收进度: " << received_ << "/" << file_size_
                      << " 字节 (" << std::fixed << std::setprecision(2) << percent << "%)" << std::flush;

            if (received_ >= file_size_) {
                outfile_.close();
                receiving_ = false;
                conn->send(std::string("UPLOAD OK\n"));
                std::cout << std::endl << "File received: " << filename_ << std::endl;

                if (minio_ && minio_->upload(filename_, filename_)) {
                    std::cout << "Upload to Minio success: " << filename_ << std::endl;
                    std::remove(filename_.c_str());
                } else {
                    std::cerr << "Upload to Minio failed: " << filename_ << std::endl;
                }
            }
        } else {
            buf->retrieveAll();
        }
    }
}

