#include "file_server_chunk.h"
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

bool parseChunkExtra_(const std::string &extra,
                      std::string &uploadId,
                      uint64_t &index,
                      uint64_t &total,
                      uint64_t &fullSize,
                      uint64_t &chunkSize)
{
    uploadId.clear();
    index = total = fullSize = chunkSize = 0;

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

    if (uploadId.empty() || sidx.empty() || st.empty() || sfull.empty() || schunk.empty())
        return false;

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

    if (total == 0 || index >= total)
        return false;
    if (chunkSize == 0)
        return false;
    return true;
}

std::string chunkBaseDir_() { return "/tmp/nimbus_chunks"; }
std::string mergedBaseDir_() { return "/tmp/nimbus_merged"; }

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

bool fileExists_(const std::string &path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

uint64_t fileSize_(const std::string &path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<uint64_t>(st.st_size);
}

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

    std::vector<char> buf(4 * 1024 * 1024);

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

    if (fullSize > 0)
        return fileExists_(mergedPath) && fileSize_(mergedPath) == fullSize;

    return true;
}

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