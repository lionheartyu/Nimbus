#include "file_server.h"
#include "file_server_util.h"
#include "file_server_chunk.h"
#include "file_server_db.h"
#include <thread>
#include <iostream>

// =====================
// 静态全局变量定义
// =====================

// 数据库配置（全局唯一）
static DbCfg g_db = {
    "127.0.0.1",     // host
    3306,            // port
    "nimbus_user",   // user
    "Nimbus@123456", // pass
    "nimbus"         // db
};

// =====================
// FileServer 构造与启动
// =====================

FileServer::FileServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
    : server_(loop, addr, name), loop_(loop)
{
    // 设置连接和消息回调
    server_.setConnectionCallback(std::bind(&FileServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(std::bind(&FileServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    server_.setThreadNum(4); // 4线程

    // 初始化 Minio 存储
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

// =====================
// 连接事件回调
// =====================

void FileServer::onConnection(const TcpConnectionPtr &conn)
{
    // 连接关闭或新连接时，重置上传状态
    if (!conn->connected())
    {
        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
        std::cout << "Connection closed." << std::endl;
    }
    else
    {
        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
        std::cout << "New connection from " << conn->peerAddress().toIpPort() << std::endl;
    }
}

// =====================
// 消息事件回调（主协议处理）
// =====================

void FileServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    // 只要缓冲区有数据就循环处理
    while (buf->readableBytes() > 0)
    {
        // ===== 1) 解析 protobuf 头 =====
        if (!pb_head_parsed_)
        {
            // 读取头长度
            if (pb_head_len_ == 0)
            {
                if (buf->readableBytes() < 4)
                    return;
                memcpy(&pb_head_len_, buf->peek(), 4);
                buf->retrieve(4);
            }

            // 读取头内容
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

            // ===== 2) 认证与登录/注册 =====
            if (head.type() == 12) // 注册
            {
                std::string err;
                if (head.filename().empty() || head.extra().empty())
                {
                    conn->send(std::string("ERROR: Empty username/password\n"));
                }
                else if (dbRegister_(g_db, head.filename(), head.extra(), err))
                {
                    conn->send(std::string("REGISTER OK\n"));
                }
                else
                {
                    conn->send(std::string("ERROR: ") + err + "\n");
                }

                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            if (head.type() == 11) // 登录
            {
                std::string token, err;
                if (head.filename().empty() || head.extra().empty())
                {
                    conn->send(std::string("ERROR: Empty username/password\n"));
                }
                else if (dbLogin_(g_db, head.filename(), head.extra(), token, err))
                {
                    conn->send(std::string("LOGIN OK token=") + token + "\n");
                }
                else
                {
                    conn->send(std::string("ERROR: ") + err + "\n");
                }

                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 登出 type=13
            if (head.type() == 13)
            {
                const std::string token = getKv_(head.extra(), "token");
                if (dbLogout_(g_db, token))
                    conn->send("LOGOUT OK\n");
                else
                    conn->send("ERROR: Logout failed\n");
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // 其余所有操作：必须带 token=xxx（在 extra 里）
            const std::string token = getKv_(head.extra(), "token");
            if (!dbCheckToken_(g_db, token))
            {
                // 部分操作需要协议格式错误响应
                if (head.type() == 2 || head.type() == 3 || head.type() == 5 || head.type() == 9)
                    sendLen0Error_(conn, "Unauthorized");
                else
                    conn->send(std::string("ERROR: Unauthorized\n"));

                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ===== 3) 按 type 分发具体业务 =====

            // ---- 云端文件列表 ----
            if (head.type() == 3)
            {
                std::vector<std::string> files;
                std::string prefix = getKv_(head.extra(), "prefix");
                bool ok = false;
                if (minio_)
                {
                    if (!prefix.empty())
                        ok = minio_->listObjectsWithPrefix(prefix, files);
                    else
                        ok = minio_->listObjects(files);
                }
                if (ok)
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
                    sendLen0Error_(conn, "List files failed");
                }

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ---- 下载文件 ----
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
                    if (!conn->connected())
                        break;

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

            // ---- 删除（移入回收站） ----
            if (head.type() == 4)
            {
                const std::string src = filename_;
                const std::string dst = std::string("recycle/") + filename_;

                conn->send(std::string("DELETE OK\n"));
                conn->shutdown();

                // 后台线程 copy+remove
                std::thread([this, src, dst]()
                            {
                    if (!minio_) return;
                    if (!minio_->copyObject(src, dst)) {
                        std::cerr << "[DELETE] copy to recycle failed: " << src << " -> " << dst << std::endl;
                        return;
                    }
                    if (!minio_->remove(src)) {
                        std::cerr << "[DELETE] remove original failed: " << src << std::endl;
                        (void)minio_->remove(dst);
                    } })
                    .detach();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ---- 列举回收站 ----
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

            // ---- 还原回收站文件 ----
            if (head.type() == 6)
            {
                const std::string recycle_name = filename_;
                std::string origin_name = recycle_name;
                if (origin_name.rfind("recycle/", 0) == 0)
                    origin_name = origin_name.substr(std::string("recycle/").size());

                conn->send(std::string("RESTORE OK\n"));
                conn->shutdown();

                // 后台线程 copy+remove
                std::thread([this, recycle_name, origin_name]()
                            {
                    if (!minio_) return;
                    if (!minio_->copyObject(recycle_name, origin_name)) {
                        std::cerr << "[RESTORE] copy failed: " << recycle_name << " -> " << origin_name << std::endl;
                        return;
                    }
                    if (!minio_->remove(recycle_name)) {
                        std::cerr << "[RESTORE] remove recycle failed: " << recycle_name << std::endl;
                        (void)minio_->remove(origin_name);
                    } })
                    .detach();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ---- 彻底删除回收站文件 ----
            if (head.type() == 7)
            {
                const std::string recycle_name = filename_;

                conn->send(std::string("REMOVE OK\n"));
                conn->shutdown();

                // 后台线程 remove
                std::thread([this, recycle_name]()
                            {
                    if (!minio_) return;
                    if (!minio_->remove(recycle_name)) {
                        std::cerr << "[REMOVE] failed: " << recycle_name << std::endl;
                    } })
                    .detach();

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ---- 生成 presigned URL ----
            if (head.type() == 8)
            {
                std::string url;
                if (minio_)
                    url = minio_->presignedUrl(filename_, 3600);

                conn->send(!url.empty() ? url : std::string("ERROR: Presigned URL failed\n"));

                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }
            // ---- 重命名文件 ----
            if (head.type() == 20)
            {
                const std::string src = filename_;
                const std::string newname = getKv_(head.extra(), "newname");
                if (newname.empty() || newname == src)
                {
                    conn->send("ERROR: newname required or same as source\n");
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }
                // 可选：防止重命名到已存在的对象（如需 exists 方法可补充）
                // if (minio_ && minio_->exists(newname)) {
                //     conn->send("ERROR: target already exists\n");
                //     conn->shutdown();
                //     resetState_(...);
                //     return;
                // }
                if (!minio_ || !minio_->copyObject(src, newname) || !minio_->remove(src))
                    conn->send("ERROR: Rename failed\n");
                else
                    conn->send("RENAME OK\n");
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }
            // ---- 预览下载 ----
            if (head.type() == 9)
            {
                uint64_t max_bytes = 2u * 1024u * 1024u;
                if (!head.extra().empty())
                {
                    const std::string mx = getKv_(head.extra(), "max");
                    if (!mx.empty())
                    {
                        try
                        {
                            max_bytes = std::stoull(mx);
                        }
                        catch (...)
                        {
                        }
                    }
                    else
                    {
                        try
                        {
                            max_bytes = std::stoull(head.extra());
                        }
                        catch (...)
                        {
                        }
                    }
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
            // ===== 3) 创建文件夹 type=22 =====
            if (head.type() == 22)
            {
                std::string dirname = getKv_(head.extra(), "dirname");
                if (dirname.empty())
                {
                    conn->send("ERROR: dirname required\n");
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }
                // 自动补全斜杠
                if (dirname.back() != '/')
                    dirname += '/';

                bool ok = false;
                if (minio_)
                    ok = minio_->uploadEmpty(dirname); // 用空内容创建“文件夹”

                if (ok)
                    conn->send("MKDIR OK\n");
                else
                    conn->send("ERROR: MKDIR failed\n");
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }

            // ---- 查询服务器磁盘空间 type=30 ----
            if (head.type() == 30)
            {
                struct statvfs vfs;
                // ★ 这里的路径请改成你的 MinIO 数据目录，比如 "/home/lion/minio/data"
                const char *path = "/home/lion/minio/data";
                if (statvfs(path, &vfs) != 0)
                {
                    conn->send("ERROR: statvfs failed\n");
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }
                uint64_t total = vfs.f_frsize * vfs.f_blocks;
                uint64_t free = vfs.f_frsize * vfs.f_bfree;
                uint64_t used = total - free;
                std::string resp = std::to_string(used) + "/" + std::to_string(total);
                conn->send(resp + "\n");
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }
            // ===== 4) 移动 type=21 =====
            if (head.type() == 21)
            {
                const std::string src = filename_;
                const std::string dst = getKv_(head.extra(), "dst");
                const std::string op = getKv_(head.extra(), "op"); // "copy" or "move"
                if (dst.empty() || op.empty())
                {
                    conn->send("ERROR: dst/op required\n");
                    conn->shutdown();
                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }
                bool ok = minio_ && minio_->copyObject(src, dst);
                if (ok && op == "move")
                    ok = minio_->remove(src);
                if (ok)
                    conn->send(op == "move" ? "MOVE OK\n" : "COPY OK\n");
                else
                    conn->send("ERROR: Move/Copy failed\n");
                conn->shutdown();
                resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                return;
            }
            // ===== 4) 大文件分片上传 type=10 =====
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

                // 记录分片元信息到 pb_head_buf_
                pb_head_buf_ = std::string("CHUNK;") + uploadId + ";" + std::to_string(index) + ";" +
                               std::to_string(total) + ";" + std::to_string(fullSize) + ";" + filename_;
            }
            else
            {
                // 普通上传，直接写文件
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

                // 空文件直接返回
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

        // ===== 5) 接收上传内容 =====
        if (receiving_)
        {
            // 写入本地文件
            size_t to_write = std::min(static_cast<uint64_t>(buf->readableBytes()), file_size_ - received_);
            outfile_.write(buf->peek(), to_write);
            buf->retrieve(to_write);
            received_ += to_write;

            // 上传完毕
            if (received_ >= file_size_)
            {
                outfile_.close();
                receiving_ = false;

                // 分片上传收完一个分片
                if (pb_head_buf_.rfind("CHUNK;", 0) == 0)
                {
                    // 解析分片元信息
                    std::string uploadId, sidx, stotal, sfull, objectName;
                    {
                        const std::string &s = pb_head_buf_;
                        size_t p1 = s.find(';');
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
                    try
                    {
                        idx = std::stoull(sidx);
                        total = std::stoull(stotal);
                        full = std::stoull(sfull);
                    }
                    catch (...)
                    {
                        conn->send(std::string("ERROR: Chunk meta invalid\n"));
                        conn->shutdown();
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

                    const std::string sessionDir = chunkBaseDir_() + "/" + uploadId;

                    // 检查所有分片是否都已收到
                    if (idx + 1 < total)
                    {
                        conn->send(std::string("CHUNK OK\n"));
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

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

                    // 合并所有分片
                    const std::string mergedPath = mergedBaseDir_() + "/" + uploadId + ".merged";
                    if (!mergeParts_(sessionDir, mergedPath, total, full))
                    {
                        conn->send(std::string("ERROR: Merge failed\n"));
                        conn->shutdown();
                        cleanupSession_(sessionDir, mergedPath, total);
                        resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                        return;
                    }

                    conn->send(std::string("UPLOAD OK\n"));
                    conn->shutdown();

                    // 后台线程上传到 Minio 并清理临时文件
                    std::thread([this, mergedPath, objectName, sessionDir, total]()
                                {
                        if (minio_ && minio_->upload(mergedPath, objectName))
                            std::remove(mergedPath.c_str());
                        cleanupSession_(sessionDir, mergedPath, total); })
                        .detach();

                    resetState_(pb_head_parsed_, pb_head_len_, pb_head_buf_, filename_, file_size_, received_, receiving_, outfile_);
                    return;
                }

                // 普通上传
                conn->send(std::string("UPLOAD OK\n"));
                std::cout << std::endl
                          << "File received: " << filename_ << std::endl;

                // 上传到 Minio，成功后删除本地文件
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
            // 未处于接收状态，丢弃数据
            buf->retrieveAll();
        }
    }
}