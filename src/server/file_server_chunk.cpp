#include "file_server_chunk.h"
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

/// 解析分片上传 extra 字符串，提取 uploadId、index、total、fullSize、chunkSize
/// @return 解析成功返回 true，否则 false
bool parseChunkExtra_(const std::string &extra,
                      std::string &uploadId,
                      uint64_t &index,
                      uint64_t &total,
                      uint64_t &fullSize,
                      uint64_t &chunkSize)
{
    uploadId.clear();
    index = total = fullSize = chunkSize = 0;

    // 辅助函数：提取 key=xxx; 形式的值
    auto get = [&](const char *key) -> std::string
    {
        std::string k(key);
        auto p = extra.find(k);
        if (p == std::string::npos)
            return "";
        p += k.size();
        auto e = extra.find(';', p);
        return extra.substr(p, (e == std::string::npos) ? std::string::npos : (e - p));
    };

    uploadId = get("uploadId=");
    std::string sidx = get("index=");
    std::string st = get("total=");
    std::string sfull = get("full=");
    std::string schunk = get("chunk=");

    // 检查字段完整性
    if (uploadId.empty() || sidx.empty() || st.empty() || sfull.empty() || schunk.empty())
        return false;

    // 转换为数值
    try
    {
        index = std::stoull(sidx);
        total = std::stoull(st);
        fullSize = std::stoull(sfull);
        chunkSize = std::stoull(schunk);
    }
    catch (...)
    {
        return false;
    }

    // 合法性检查
    if (total == 0 || index >= total)
        return false;
    if (chunkSize == 0)
        return false;
    return true;
}

/// 分片临时存储目录
std::string chunkBaseDir_() { return "/tmp/nimbus_chunks"; }

/// 合并后文件存储目录
std::string mergedBaseDir_() { return "/tmp/nimbus_merged"; }

/// 确保目录存在（递归创建）
bool ensureDir_(const std::string &path)
{
    if (path.empty())
        return false;
    if (::access(path.c_str(), F_OK) == 0)
        return true;

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

/// 判断文件是否存在
bool fileExists_(const std::string &path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

/// 获取文件大小（字节），失败返回0
uint64_t fileSize_(const std::string &path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<uint64_t>(st.st_size);
}

/// 合并所有分片为一个完整文件
/// @param sessionDir 分片目录
/// @param mergedPath 合并后文件路径
/// @param totalParts 分片总数
/// @param fullSize   合并后文件应有大小（用于校验）
bool mergeParts_(const std::string &sessionDir,
                 const std::string &mergedPath,
                 uint64_t totalParts,
                 uint64_t fullSize)
{
    if (!ensureDir_(mergedBaseDir_()))
        return false;

    std::ofstream out(mergedPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    std::vector<char> buf(4 * 1024 * 1024); // 4MB缓冲区

    for (uint64_t i = 0; i < totalParts; ++i)
    {
        const std::string partPath = sessionDir + "/" + std::to_string(i) + ".part";
        std::ifstream in(partPath, std::ios::binary);
        if (!in)
            return false;

        while (in)
        {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const std::streamsize n = in.gcount();
            if (n > 0)
                out.write(buf.data(), n);
        }
        if (!out)
            return false;
    }

    out.flush();
    out.close();

    // 校验合并后文件大小
    if (fullSize > 0)
        return fileExists_(mergedPath) && fileSize_(mergedPath) == fullSize;

    return true;
}

/// 清理分片会话目录和合并文件
void cleanupSession_(const std::string &sessionDir, const std::string &mergedPath, uint64_t totalParts)
{
    std::remove(mergedPath.c_str());
    for (uint64_t i = 0; i < totalParts; ++i)
    {
        const std::string partPath = sessionDir + "/" + std::to_string(i) + ".part";
        std::remove(partPath.c_str());
    }
    ::rmdir(sessionDir.c_str());
}