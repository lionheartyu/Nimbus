#include "file_server_util.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <random>
#include <openssl/sha.h>

/// 重置文件上传状态，关闭文件流并清空相关变量
void resetState_(bool &pb_head_parsed,
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

/// 发送长度为0的错误响应（协议规定），附带错误消息
void sendLen0Error_(const TcpConnectionPtr &conn, const std::string &msg)
{
    uint32_t len = 0;
    conn->send(std::string(reinterpret_cast<const char *>(&len), 4));
    conn->send(std::string("ERROR: ") + msg + "\n");
}

/// 从extra字符串中提取key对应的值（格式 key=xxx;）
std::string getKv_(const std::string &extra, const std::string &key)
{
    const std::string k = key + "=";
    size_t p = extra.find(k);
    if (p == std::string::npos)
        return "";
    p += k.size();
    size_t e = extra.find(';', p);
    return extra.substr(p, (e == std::string::npos) ? std::string::npos : (e - p));
}

/// 生成UUID字符串（伪随机，非标准实现）
std::string genUuid_()
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    auto hex = [](uint64_t x, int n)
    {
        std::ostringstream os;
        os << std::hex << std::setfill('0') << std::nouppercase;
        os << std::setw(n) << (x & ((1ULL << (n * 4)) - 1));
        return os.str();
    };

    const uint64_t a = rng();
    const uint64_t b = rng();
    return hex(a >> 32, 8) + "-" + hex(a >> 16, 4) + "-" + hex(a, 4) + "-" + hex(b >> 48, 4) + "-" + hex(b, 12);
}

/// 计算数据的SHA256哈希
std::vector<unsigned char> sha256_(const std::vector<unsigned char> &data)
{
    std::vector<unsigned char> out(SHA256_DIGEST_LENGTH);
    SHA256_CTX ctx{};
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data.data(), data.size());
    SHA256_Final(out.data(), &ctx);
    return out;
}

/// 生成n字节的随机数据
std::vector<unsigned char> randomBytes_(size_t n)
{
    std::vector<unsigned char> b(n);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < n; ++i)
        b[i] = static_cast<unsigned char>(dist(rng));
    return b;
}

/// 字节数组转16进制字符串
std::string toHex_(const std::vector<unsigned char> &b)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (unsigned char c : b)
        os << std::setw(2) << (int)c;
    return os.str();
}

/// 16进制字符串转字节数组
std::vector<unsigned char> hexToBytes_(const std::string &hx)
{
    std::vector<unsigned char> b;
    if (hx.size() % 2)
        return b;
    b.reserve(hx.size() / 2);
    for (size_t i = 0; i < hx.size(); i += 2)
    {
        unsigned int v = 0;
        std::stringstream ss;
        ss << std::hex << hx.substr(i, 2);
        ss >> v;
        b.push_back(static_cast<unsigned char>(v));
    }
    return b;
}