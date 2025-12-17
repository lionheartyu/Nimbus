#pragma once
#include <mymuduo/TcpServer.h>
#include <string>
#include <mymuduo/logger.h>
#include <functional>

class EchoServer
{
public:
    EchoServer(EventLoop *loop, const InetAddress &addr, const std::string &name);
    void start();

private:
    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time);

    EventLoop *loop_;
    TcpServer server_;
};