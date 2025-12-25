#include "file_server.h"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <thread>   // add: async delete/restore/remove

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

// ===== 分片上传（type=10，选A：最后一片触发合并并上传）=====
// extra: "uploadId=xxx;index=i;total=n;full=bytes;chunk=bytes"
static bool parseChunkExtra_(const std::string& extra,
                            std::string& uploadId,
                            uint64_t& index,
                            uint64_t& total,
                            uint64_t& fullSize,
                            uint64_t& chunkSize)
{
    uploadId.clear();
    index = total = fullSize = chunkSize = 0;

    auto get = [&](const char* key) -> std::string {
        std::string k(key);
        auto p = extra.find(k);
        if (p == std::string::npos) return "";
        p += k.size();
        auto e = extra.find(';', p);
        return extra.substr(p, (e == std::string::npos) ? std::string::npos : (e - p));
    };

    uploadId = get("uploadId=");
    std::string sidx = get("index=");
    std::string st  = get("total=");
    std::string sfull = get("full=");
    std::string schunk = get("chunk=");

    if (uploadId.empty() || sidx.empty() || st.empty() || sfull.empty() || schunk.empty())
        return false;

    try {
        index = std::stoull(sidx);
        total = std::stoull(st);
        fullSize = std::stoull(sfull);
        chunkSize = std::stoull(schunk);
    } catch (...) {
        return false;
    }

    if (total == 0 || index >= total) return false;
    if (chunkSize == 0) return false;
    return true;
}

// ===== 删除 std::filesystem 版，替换为 POSIX 版 =====
static inline std::string chunkBaseDir_()  { return "/tmp/nimbus_chunks"; }
static inline std::string mergedBaseDir_() { return "/tmp/nimbus_merged"; }

static bool ensureDir_(const std::string& path)
{
    // 递归 mkdir -p
    if (path.empty()) return false;
    if (::access(path.c_str(), F_OK) == 0) return true;

    std::string cur;
    cur.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i)
    {
        char c = path[i];
        cur.push_back(c);
        if (c == '/' && cur.size() > 1)
        {
            if (::access(cur.c_str(), F_OK) != 0)
            {
                if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            }
        }
    }
    if (::access(cur.c_str(), F_OK) != 0)
    {
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

static bool fileExists_(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

static uint64_t fileSize_(const std::string& path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

static bool mergeParts_(const std::string& sessionDir,
                        const std::string& mergedPath,
                        uint64_t totalParts,
                        uint64_t fullSize)
{
    // 确保 merged 目录存在
    if (!ensureDir_(mergedBaseDir_())) return false;

    std::ofstream out(mergedPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    std::vector<char> buf(4 * 1024 * 1024);

    for (uint64_t i = 0; i < totalParts; ++i)
    {
        const std::string partPath = sessionDir + "/" + std::to_string(i) + ".part";
        std::ifstream in(partPath, std::ios::binary);
        if (!in) return false;

        while (in)
        {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize n = in.gcount();
            if (n > 0) out.write(buf.data(), n);
        }
        if (!out) return false;
    }

    out.flush();
    out.close();

    if (fullSize > 0)
        return fileExists_(mergedPath) && fileSize_(mergedPath) == fullSize;

    return true;
}

static void cleanupSession_(const std::string& sessionDir, const std::string& mergedPath, uint64_t totalParts)
{
    // 删 merged
    std::remove(mergedPath.c_str());

    // 删分片
    for (uint64_t i = 0; i < totalParts; ++i)
    {
        const std::string partPath = sessionDir + "/" + std::to_string(i) + ".part";
        std::remove(partPath.c_str());
    }
    // 删目录（可能失败，不强求）
    ::rmdir(sessionDir.c_str());
}

} // namespace

FileServer::FileServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
    : server_(loop, addr, name), loop_(loop)
{
    // 设置连接建立/断开时的回调
    server_.setConnectionCallback(std::bind(&FileServer::onConnection, this, std::placeholders::_1));
    // 设置消息到达时的回调
    server_.setMessageCallback(std::bind(&FileServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // fix: 你当前解析/上传状态是成员变量（跨连接共享），多线程必串状态导致随机失败（还原/上传/删除都会“偶现失败”）
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

            FileHeader head;
            if (!head.ParseFromString(pb_head_buf_))
            {
                conn->send(std::string("ERROR: Protobuf parse failed\n"));
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            filename_ = head.filename();
            file_size_ = head.filesize();
            pb_head_parsed_ = true;

            // ===== 2) 按 type 分发 =====

            // 列举云端文件
            if (head.type() == 3)
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
            if (head.type() == 2)
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

            // 删除（移入回收站）—— fix: 大文件 copy 很慢，客户端会超时；改为先回 OK 再后台做 copy/remove
            if (head.type() == 4)
            {
                const std::string src = filename_;
                const std::string dst = std::string("recycle/") + filename_;

                conn->send(std::string("DELETE OK\n"));
                conn->shutdown();

                std::thread([this, src, dst]() {
                    if (!minio_) return;
                    const bool ok = minio_->copyObject(src, dst) && minio_->remove(src);
                    if (!ok) std::cerr << "Async delete failed: " << src << " -> " << dst << std::endl;
                }).detach();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 列举回收站
            if (head.type() == 5)
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

            // 还原回收站文件 —— fix: 同样先回 OK 再后台 copy/remove（避免大文件还原超时）
            if (head.type() == 6)
            {
                std::string origin_name = head.extra().empty() ? filename_ : head.extra();
                // 防御：不允许还原目标仍在 recycle/ 下
                if (origin_name.rfind("recycle/", 0) == 0)
                    origin_name = origin_name.substr(std::string("recycle/").size());

                const std::string recycle_name = std::string("recycle/") + filename_;

                conn->send(std::string("RESTORE OK\n"));
                conn->shutdown();

                std::thread([this, recycle_name, origin_name]() {
                    if (!minio_) return;
                    const bool ok = minio_->copyObject(recycle_name, origin_name) && minio_->remove(recycle_name);
                    if (!ok) std::cerr << "Async restore failed: " << recycle_name << " -> " << origin_name << std::endl;
                }).detach();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 彻底删除回收站文件 —— fix: 先回 OK 再后台 remove（避免大文件删除超时）
            if (head.type() == 7)
            {
                const std::string recycle_name = std::string("recycle/") + filename_;

                conn->send(std::string("REMOVE OK\n"));
                conn->shutdown();

                std::thread([this, recycle_name]() {
                    if (!minio_) return;
                    const bool ok = minio_->remove(recycle_name);
                    if (!ok) std::cerr << "Async remove failed: " << recycle_name << std::endl;
                }).detach();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 生成 presigned URL（文本返回）
            if (head.type() == 8)
            {
                std::string url;
                if (minio_)
                    url = minio_->presignedUrl(filename_, 3600);

                conn->send(!url.empty() ? url : std::string("ERROR: Presigned URL failed\n"));

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ====== 新增：type=9 预览下载（小文件：图片/文本）======
            if (head.type() == 9)
            {
                // extra 可选：客户端可以传一个最大字节数
                // 若 proto 没有 extra 字段或客户端没传也无所谓
                uint64_t max_bytes = 2u * 1024u * 1024u; // 默认 2MB（图片/文本足够）
                if (!head.extra().empty())
                {
                    try { max_bytes = std::stoull(head.extra()); } catch (...) {}
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

            // ===== type=10: 大文件分片上传（选A：最后一片触发合并并上传）=====
            if (head.type() == 10)
            {
                std::string uploadId;
                uint64_t index = 0, total = 0, fullSize = 0, chunkSize = 0;
                if (!parseChunkExtra_(head.extra(), uploadId, index, total, fullSize, chunkSize))
                {
                    conn->send(std::string("ERROR: Bad chunk extra\n"));
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                const std::string sessionDir = chunkBaseDir_() + "/" + uploadId;
                if (!ensureDir_(sessionDir))
                {
                    conn->send(std::string("ERROR: Cannot create chunk dir\n"));
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                const std::string partPath = sessionDir + "/" + std::to_string(index) + ".part";
                outfile_.open(partPath, std::ios::binary | std::ios::trunc);
                if (!outfile_)
                {
                    conn->send(std::string("ERROR: Cannot open part file\n"));
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                receiving_ = true;
                received_ = 0;
                file_size_ = chunkSize;

                // 用 filename_ 保存 objectName；pb_head_buf_ 改成分片元信息用于完成时合并上传
                // 格式：CHUNK;uploadId;index;total;full;objectName
                pb_head_buf_ = std::string("CHUNK;") + uploadId + ";" + std::to_string(index) + ";" +
                              std::to_string(total) + ";" + std::to_string(fullSize) + ";" + filename_;

                // 不 return：继续走 receiving_ 分支写入当前 buf 里的数据
            }
            else
            {
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

                if (file_size_ == 0)
                {
                    outfile_.close();
                    receiving_ = false;
                    conn->send(std::string("UPLOAD OK\n"));

                    if (minio_ && minio_->upload(filename_, filename_))
                        std::remove(filename_.c_str());

                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }
            }
        }

        // ===== 4) 接收上传内容 =====
        if (receiving_)
        {
            size_t to_write = std::min(static_cast<uint64_t>(buf->readableBytes()), file_size_ - received_);
            outfile_.write(buf->peek(), to_write);
            buf->retrieve(to_write);
            received_ += to_write;

            if (received_ >= file_size_)
            {
                outfile_.close();
                receiving_ = false;

                // fix: 分片完成时不能回 UPLOAD OK（除最后一片），并且最后一片要合并+上传
                if (pb_head_buf_.rfind("CHUNK;", 0) == 0)
                {
                    // 解析：CHUNK;uploadId;index;total;full;objectName
                    std::string uploadId, sidx, stotal, sfull, objectName;
                    {
                        const std::string& s = pb_head_buf_;
                        size_t p1 = s.find(';');            // after CHUNK
                        size_t p2 = s.find(';', p1 + 1);
                        size_t p3 = s.find(';', p2 + 1);
                        size_t p4 = s.find(';', p3 + 1);
                        size_t p5 = s.find(';', p4 + 1);
                        if (p5 == std::string::npos)
                        {
                            conn->send(std::string("ERROR: Chunk meta parse failed\n"));
                            conn->shutdown();
                            resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                            return;
                        }
                        uploadId = s.substr(p1 + 1, p2 - p1 - 1);
                        sidx = s.substr(p2 + 1, p3 - p2 - 1);
                        stotal = s.substr(p3 + 1, p4 - p3 - 1);
                        sfull = s.substr(p4 + 1, p5 - p4 - 1);
                        objectName = s.substr(p5 + 1);
                    }

                    uint64_t idx = 0, total = 0, full = 0;
                    try {
                        idx = std::stoull(sidx);
                        total = std::stoull(stotal);
                        full = std::stoull(sfull);
                    } catch (...) {
                        conn->send(std::string("ERROR: Chunk meta invalid\n"));
                        conn->shutdown();
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

                    const std::string sessionDir = chunkBaseDir_() + "/" + uploadId;

                    // 不是最后一片：回 CHUNK OK
                    if (idx + 1 < total)
                    {
                        conn->send(std::string("CHUNK OK\n"));
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

                    // 最后一片：检查齐全 -> 合并 -> 上传
                    for (uint64_t i = 0; i < total; ++i)
                    {
                        const std::string partPath = sessionDir + "/" + std::to_string(i) + ".part";
                        if (!fileExists_(partPath))
                        {
                            conn->send(std::string("ERROR: Missing part ") + std::to_string(i) + "\n");
                            conn->shutdown();
                            resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                            return;
                        }
                    }

                    const std::string mergedPath = mergedBaseDir_() + "/" + uploadId + ".merged";
                    if (!mergeParts_(sessionDir, mergedPath, total, full))
                    {
                        conn->send(std::string("ERROR: Merge failed\n"));
                        conn->shutdown();
                        cleanupSession_(sessionDir, mergedPath, total);
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

                    const bool ok = minio_ && minio_->upload(mergedPath, objectName);
                    if (!ok)
                    {
                        conn->send(std::string("ERROR: Upload to MinIO failed\n"));
                        conn->shutdown();
                        cleanupSession_(sessionDir, mergedPath, total);
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

                    conn->send(std::string("UPLOAD OK\n"));
                    cleanupSession_(sessionDir, mergedPath, total);

                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                // 普通上传完成（你原逻辑）
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
