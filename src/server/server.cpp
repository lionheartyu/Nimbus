#include "server.h"

// 构造函数，初始化 TcpServer 并设置回调函数
EchoServer::EchoServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
    : server_(loop, addr, name),
      loop_(loop)
{
    // 设置连接建立/断开时的回调
    server_.setConnectionCallback(std::bind(&EchoServer::onConnection, this, std::placeholders::_1));
    // 设置消息到达时的回调
    server_.setMessageCallback(std::bind(&EchoServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    // 设置工作线程数量
    server_.setThreadNum(3);
}

// 启动服务器
void EchoServer::start()
{
    server_.start();
}

// 连接建立或断开时的回调函数
void EchoServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        // 新连接建立
        LOG_INFO("conn up:%s", conn->peerAddress().toIpPort().c_str());
    }
    else
    {
        // 连接断开
        LOG_INFO("conn down:%s", conn->peerAddress().toIpPort().c_str());
    }
}

// 消息到达时的回调函数
void EchoServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
{
    // 读取收到的全部数据
    std::string msg = buf->retrieveAllAsString();
    // 原样返回给客户端（回声功能）
    conn->send(msg);
    // 不关闭连接，允许多次收发
}