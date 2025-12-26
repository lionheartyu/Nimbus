#pragma once
#include <string>
#include <fstream>
#include <mymuduo/TcpConnection.h>

/// 重置文件上传状态，关闭文件流并清空相关变量
/// @param pb_head_parsed  是否已解析protobuf头
/// @param pb_head_len     protobuf头长度
/// @param pb_head_buf     protobuf头内容
/// @param filename        文件名
/// @param file_size       文件总大小
/// @param received        已接收字节数
/// @param receiving       是否正在接收
/// @param outfile         文件输出流
void resetState_(bool &pb_head_parsed,
                 uint32_t &pb_head_len,
                 std::string &pb_head_buf,
                 std::string &filename,
                 uint64_t &file_size,
                 uint64_t &received,
                 bool &receiving,
                 std::ofstream &outfile);

/// 发送长度为0的错误响应（协议规定），附带错误消息
/// @param conn 连接对象
/// @param msg  错误消息
void sendLen0Error_(const TcpConnectionPtr &conn, const std::string &msg);

/// 从extra字符串中提取key对应的值（格式 key=xxx;）
/// @param extra 额外信息字符串
/// @param key   要查找的键
/// @return      对应的值，未找到返回空字符串
std::string getKv_(const std::string &extra, const std::string &key);

/// 生成UUID字符串（伪随机，非标准实现）
/// @return UUID字符串
std::string genUuid_();

/// 计算数据的SHA256哈希
/// @param data 输入数据
/// @return     SHA256哈希值（字节数组）
std::vector<unsigned char> sha256_(const std::vector<unsigned char> &data);

/// 生成n字节的随机数据
/// @param n 字节数
/// @return  随机字节数组
std::vector<unsigned char> randomBytes_(size_t n);

/// 字节数组转16进制字符串
/// @param b 字节数组
/// @return  16进制字符串
std::string toHex_(const std::vector<unsigned char> &b);

/// 16进制字符串转字节数组
/// @param hx 16进制字符串
/// @return   字节数组
std::vector<unsigned char> hexToBytes_(const std::string &hx);