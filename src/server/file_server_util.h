#pragma once
#include <string>
#include <fstream>
#include<mymuduo/TcpConnection.h>

void resetState_(bool &pb_head_parsed,
                 uint32_t &pb_head_len,
                 std::string &pb_head_buf,
                 std::string &filename,
                 uint64_t &file_size,
                 uint64_t &received,
                 bool &receiving,
                 std::ofstream &outfile);

void sendLen0Error_(const TcpConnectionPtr &conn, const std::string &msg);
std::string getKv_(const std::string &extra, const std::string &key);
std::string genUuid_();
std::vector<unsigned char> sha256_(const std::vector<unsigned char> &data);
std::vector<unsigned char> randomBytes_(size_t n);
std::string toHex_(const std::vector<unsigned char> &b);
std::vector<unsigned char> hexToBytes_(const std::string &hx);