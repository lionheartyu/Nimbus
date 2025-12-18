#include <mymuduo/TcpServer.h>
#include <string>
#include <mymuduo/logger.h>
#include <functional>

class EchoServer
{
public:
    EchoServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
        : server_(loop, addr, name),
          loop_(loop)
    {
        // 注册回调函数
        server_.setConnectionCallback(std::bind(&EchoServer::onConnection, this, std::placeholders::_1));

        server_.setMessageCallback(std::bind(&EchoServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        // 设置合适的loop线程数量  loopthread
        server_.setThreadNum(3);
    }
    
    void start()
    {
        server_.start();
    }

private:
    // 连接建立或者断开的回调
    void onConnection(const TcpConnectionPtr &conn)
    {
        if (conn->connected())
        {
            LOG_INFO("conn up:%s", conn->peerAddress().toIpPort().c_str());
        }
        else
        {
            LOG_INFO("conn down:%s", conn->peerAddress().toIpPort().c_str());
        }
    }

    // 可读写事件回调
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
    {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
        conn->shutdown(); // 写端 EPOLLHUP => closecallback
    }

    EventLoop *loop_;
    TcpServer server_;
};

int main()
{
    EventLoop loop;
    InetAddress addr(8080, "10.20.32.132");
    EchoServer echoServer(&loop, addr, "myserver-01"); // accpertor non-blocking listenfd create bind
    echoServer.start();                                // listen  loopthread  listenfd => accpetorchannle => mainloop
    loop.loop();                                       // 启动mainloop的底层poller
    return 0;
}