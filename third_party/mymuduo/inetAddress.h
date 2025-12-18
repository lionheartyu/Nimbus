#pragma once
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
// 封装socket地址类型
class InetAddress
{
public:
    explicit InetAddress(uint16_t port = 0, std::string ip = "192.168.217.148");
    explicit InetAddress(const sockaddr_in &addr)
        : addr_(addr) {};
    std::string toIP() const;
    std::string toIpPort() const;
    std::uint16_t toPort() const;

    const sockaddr_in *getSockAddr() const { return &addr_; }
    void setSockAddr(const sockaddr_in &addr) { addr_ = addr; }

private:
    sockaddr_in addr_;
};