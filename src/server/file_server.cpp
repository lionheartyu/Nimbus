#include "file_server.h"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <csignal> // add

namespace {

// 忽略 SIGPIPE：防止客户端提前断开导致服务端进程退出
struct IgnoreSigPipe_ {
    IgnoreSigPipe_() { ::signal(SIGPIPE, SIG_IGN); }
};
static IgnoreSigPipe_ g_ignore_sigpipe;

// 每次处理完一个请求后统一重置（保持你现有“一个连接处理一个请求”的方式）
static inline void resetState_(bool &pb_head_parsed,
                               uint32_t &pb_head_len,
                               std::string &pb_head_buf,
                               std::string &filename,
                               uint64_t &file_size,
                               uint64_t &received,
                               bool &receiving,
                               std::ofstream &outfile)
{
    pb_head_parsed = false;
    pb_head_len = 0;
    pb_head_buf.clear();
    filename.clear();
    file_size = 0;
    received = 0;

    if (outfile.is_open())
        outfile.close();
    receiving = false;
}

// 下载失败时不要直接发 ERROR 文本（会让客户端把前4字节当长度读，导致“长度异常/乱码”）
// 统一为：先发4字节 0，再发错误文本
static inline void sendLen0Error_(const TcpConnectionPtr &conn, const std::string &msg)
{
    uint32_t len = 0;
    conn->send(std::string(reinterpret_cast<const char *>(&len), 4));
    conn->send(std::string("ERROR: ") + msg + "\n");
}

} // namespace

FileServer::FileServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
    : server_(loop, addr, name), loop_(loop)
{
    // 设置连接建立/断开时的回调
    server_.setConnectionCallback(std::bind(&FileServer::onConnection, this, std::placeholders::_1));
    // 设置消息到达时的回调
    server_.setMessageCallback(std::bind(&FileServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // 重要：你现在解析/上传状态是成员变量（跨连接共享），多线程会串状态导致乱码/协议错位
    server_.setThreadNum(1);

    minio_ = std::make_unique<MinioStorage>(
        "127.0.0.1:9000",
        "minioadmin",
        "minioadmin",
        "data");
}

void FileServer::start()
{
    server_.start();
}

// 连接建立或断开时的回调函数
void FileServer::onConnection(const TcpConnectionPtr &conn)
{
    if (!conn->connected())
    {
        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
        std::cout << "Connection closed." << std::endl;
    }
    else
    {
        // 新连接建立也重置一次，避免上次连接遗留状态污染
        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
        std::cout << "New connection from " << conn->peerAddress().toIpPort() << std::endl;
    }
}

// 消息到达时的回调函数
void FileServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    while (buf->readableBytes() > 0)
    {
        if (!pb_head_parsed_)
        {
            // ===== 1) 解析 protobuf 头 =====
            if (pb_head_len_ == 0)
            {
                if (buf->readableBytes() < 4)
                    return;
                memcpy(&pb_head_len_, buf->peek(), 4);
                buf->retrieve(4);
            }

            if (buf->readableBytes() < pb_head_len_)
                return;

            pb_head_buf_.assign(buf->peek(), pb_head_len_);
            buf->retrieve(pb_head_len_);

            FileHeader header;
            if (!header.ParseFromString(pb_head_buf_))
            {
                conn->send(std::string("ERROR: Protobuf parse failed\n"));
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            filename_ = header.filename();
            file_size_ = header.filesize();
            pb_head_parsed_ = true;

            // ===== 2) 按 type 分发 =====

            // 列举云端文件
            if (header.type() == 3)
            {
                std::vector<std::string> files;
                if (minio_ && minio_->listObjects(files))
                {
                    ListFilesResponse resp;
                    for (const auto &f : files)
                        resp.add_filenames(f);

                    std::string out;
                    resp.SerializeToString(&out);

                    uint32_t len = static_cast<uint32_t>(out.size());
                    conn->send(std::string(reinterpret_cast<const char *>(&len), 4));
                    conn->send(out);
                }
                else
                {
                    // 列表接口目前客户端按 [len][pb] 读，这里也按统一格式返回错误（len=0 + 文本）
                    sendLen0Error_(conn, "List files failed");
                }

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 下载文件（统一改成失败也先发 len）
            if (header.type() == 2)
            {
                std::string tmp_path = "/tmp/" + filename_;
                bool ok = minio_ && minio_->download(filename_, tmp_path);
                if (!ok)
                {
                    sendLen0Error_(conn, "Download from Minio failed");
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                std::ifstream infile(tmp_path, std::ios::binary | std::ios::ate);
                if (!infile)
                {
                    sendLen0Error_(conn, "Cannot open downloaded file");
                    conn->shutdown();
                    std::remove(tmp_path.c_str());
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                std::streamsize filesize = infile.tellg();
                infile.seekg(0, std::ios::beg);

                uint32_t len = static_cast<uint32_t>(filesize);
                conn->send(std::string(reinterpret_cast<const char *>(&len), 4));

                char buf4k[4096];
                while (infile)
                {
                    if (!conn->connected()) break; // 关键：对端断了就别再 send

                    infile.read(buf4k, sizeof(buf4k));
                    std::streamsize n = infile.gcount();
                    if (n > 0)
                        conn->send(std::string(buf4k, static_cast<size_t>(n)));
                }

                infile.close();
                std::remove(tmp_path.c_str());

                if (conn->connected())
                    conn->shutdown();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 删除（移入回收站）
            if (header.type() == 4)
            {
                std::string recycle_name = std::string("recycle/") + filename_;
                bool ok = false;
                if (minio_)
                {
                    ok = minio_->copyObject(filename_, recycle_name) && minio_->remove(filename_);
                }
                conn->send(ok ? std::string("DELETE OK\n") : std::string("ERROR: Delete to recycle failed\n"));

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 列举回收站
            if (header.type() == 5)
            {
                std::vector<std::string> files;
                if (minio_ && minio_->listObjectsWithPrefix("recycle/", files))
                {
                    ListFilesResponse resp;
                    for (const auto &f : files)
                        resp.add_filenames(f);

                    std::string out;
                    resp.SerializeToString(&out);

                    uint32_t len = static_cast<uint32_t>(out.size());
                    conn->send(std::string(reinterpret_cast<const char *>(&len), 4));
                    conn->send(out);
                }
                else
                {
                    sendLen0Error_(conn, "List recycle failed");
                }

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 还原回收站文件
            if (header.type() == 6)
            {
                std::string origin_name = header.extra().empty() ? filename_ : header.extra();
                std::string recycle_name = std::string("recycle/") + filename_;
                bool ok = false;
                if (minio_)
                {
                    ok = minio_->copyObject(recycle_name, origin_name) && minio_->remove(recycle_name);
                }
                conn->send(ok ? std::string("RESTORE OK\n") : std::string("ERROR: Restore failed\n"));

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 彻底删除回收站文件
            if (header.type() == 7)
            {
                std::string recycle_name = std::string("recycle/") + filename_;
                bool ok = minio_ && minio_->remove(recycle_name);
                conn->send(ok ? std::string("REMOVE OK\n") : std::string("ERROR: Remove failed\n"));

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 生成 presigned URL（文本返回）
            if (header.type() == 8)
            {
                std::string url;
                if (minio_)
                    url = minio_->presignedUrl(filename_, 3600);

                conn->send(!url.empty() ? url : std::string("ERROR: Presigned URL failed\n"));

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ====== 新增：type=9 预览下载（小文件：图片/文本）======
            if (header.type() == 9)
            {
                // extra 可选：客户端可以传一个最大字节数
                // 若 proto 没有 extra 字段或客户端没传也无所谓
                uint64_t max_bytes = 2u * 1024u * 1024u; // 默认 2MB（图片/文本足够）
                if (!header.extra().empty())
                {
                    try { max_bytes = std::stoull(header.extra()); } catch (...) {}
                }

                std::string tmp_path = "/tmp/" + filename_;
                bool ok = minio_ && minio_->download(filename_, tmp_path);
                if (!ok)
                {
                    sendLen0Error_(conn, "Preview download from Minio failed");
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                std::ifstream infile(tmp_path, std::ios::binary | std::ios::ate);
                if (!infile)
                {
                    sendLen0Error_(conn, "Cannot open preview temp file");
                    conn->shutdown();
                    std::remove(tmp_path.c_str());
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                std::streamsize filesize = infile.tellg();
                infile.seekg(0, std::ios::beg);

                if (filesize < 0)
                {
                    sendLen0Error_(conn, "Invalid preview file size");
                    conn->shutdown();
                    infile.close();
                    std::remove(tmp_path.c_str());
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                if (static_cast<uint64_t>(filesize) > max_bytes)
                {
                    sendLen0Error_(conn, "Preview too large");
                    conn->shutdown();
                    infile.close();
                    std::remove(tmp_path.c_str());
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                uint32_t len = static_cast<uint32_t>(filesize);
                conn->send(std::string(reinterpret_cast<const char *>(&len), 4));

                std::string data;
                data.resize(len);
                if (len > 0)
                {
                    infile.read(&data[0], len);
                    if (!infile)
                    {
                        sendLen0Error_(conn, "Preview read failed");
                        conn->shutdown();
                        infile.close();
                        std::remove(tmp_path.c_str());
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }
                    conn->send(data);
                }

                infile.close();
                std::remove(tmp_path.c_str());
                conn->shutdown();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ===== 3) 默认当上传处理 =====
            outfile_.open(filename_, std::ios::binary);
            if (!outfile_)
            {
                conn->send(std::string("ERROR: Cannot open file\n"));
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            receiving_ = true;
            received_ = 0;
            std::cout << "Start receiving file: " << filename_ << ", size: " << file_size_ << std::endl;

            // 空文件：直接完成并上传到 MinIO
            if (file_size_ == 0)
            {
                outfile_.close();
                receiving_ = false;
                conn->send(std::string("UPLOAD OK\n"));

                if (minio_ && minio_->upload(filename_, filename_))
                {
                    std::cout << "Upload to Minio success: " << filename_ << std::endl;
                    std::remove(filename_.c_str());
                }
                else
                {
                    std::cerr << "Upload to Minio failed: " << filename_ << std::endl;
                }

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }
        }
        // ===== 4) 接收上传内容 =====
        else if (receiving_)
        {
            size_t to_write = std::min(static_cast<uint64_t>(buf->readableBytes()), file_size_ - received_);
            outfile_.write(buf->peek(), to_write);
            buf->retrieve(to_write);
            received_ += to_write;

            double percent = file_size_ ? (100.0 * received_ / file_size_) : 100.0;
            std::cout << "\r接收进度: " << received_ << "/" << file_size_
                      << " 字节 (" << std::fixed << std::setprecision(2) << percent << "%)" << std::flush;

            if (received_ >= file_size_)
            {
                outfile_.close();
                receiving_ = false;
                conn->send(std::string("UPLOAD OK\n"));
                std::cout << std::endl << "File received: " << filename_ << std::endl;

                if (minio_ && minio_->upload(filename_, filename_))
                {
                    std::cout << "Upload to Minio success: " << filename_ << std::endl;
                    std::remove(filename_.c_str());
                }
                else
                {
                    std::cerr << "Upload to Minio failed: " << filename_ << std::endl;
                }

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }
        }
        else
        {
            buf->retrieveAll();
        }
    }
}
