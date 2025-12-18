#include "TcpServer.h"
#include "logger.h"
#include <functional>
#include <strings.h>
#include "TcpConnection.h"
static EventLoop *checkloopnotnull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainloop is null\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpServer::TcpServer(EventLoop *loop,
                     const InetAddress &listenAddr,
                     const std::string &nameArg,
                     Option option)
    : loop_(checkloopnotnull(loop)),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(new Acceptor(loop_, listenAddr, option == kNoReusePort)),
      threadPool_(new EventLoopThreadPoll(loop_, name_)),
      connectionCallback_(),
      messageCallback_(),
      nextConnId_(1),
      started_(0)
{
    // 当有新用户连接时,会执行TcpServer::newConnection回调
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
                                                  std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer()
{
    for(auto &item : connections_)
    {
        //这个局部的shared_ptr智能指针对象, 出右括号, 可以自动释放new出来的TcpConnection对象资源了
        TcpConnectionPtr conn(item.second);
        item.second.reset();

        //销毁连接
         conn->getLoop()->runInLoop(std::bind(&TcpConnection::connectionDestroyed,conn));
    }
}

// 设置底层subloop的个数
void TcpServer::setThreadNum(int numThreads)
{
    threadPool_->setThreadNum(numThreads);
}
//   mainloop 的loop.loop()事件监听  由用户自己开启
//   新连接的loop.loop() 的事件监听 在启动一个线程的时候===> one thread per loop 的时候就启动了
// 开启服务器监听   loop.loop()  poller开始运行
void TcpServer::start()
{
    if (started_++ == 0)
    {
        threadPool_->start(threadInitCallback_); // 启动底层的loop线程池
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

// 有一个新的客户端的连接 acceptor 会执行这个回调操作
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{
    // 轮询算法,选择一个subloop,来管理channel
    EventLoop *ioloop = threadPool_->getNextLoop();
    char buf[64] = {0};
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
    nextConnId_++;
    std::string connName = name_ + buf;

    LOG_INFO("TcpServer::newConnection[%s] - new connection[%s] from %s \n",
             name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());

    // 通过sockfd 获取 其 绑定的  ===> 本机的ip地址和端口号
    sockaddr_in local;
    ::bzero(&local, sizeof local);
    socklen_t addrlen = sizeof local;
    if (::getsockname(sockfd, (sockaddr *)&local, &addrlen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr");
    }
    InetAddress localAddr(local);

    // 根据连接成功的sockfd 创建TcpConnection连接对象
    TcpConnectionPtr conn(new TcpConnection(ioloop, connName, sockfd, localAddr, peerAddr));
    connections_[connName] = conn;
    // 下面的回调都是用户设置给TcpServer=>TcpConnection=>channel=>poller=>notify channel 调用回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // 设置了如何关闭连接的回调  conn->shutDown()
    conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

    // 直接调用&TcpConnection::connectionEstablished
    ioloop->runInLoop(std::bind(&TcpConnection::connectionEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop[%s] - connection %s \n", name_.c_str(), conn->name().c_str());

    connections_.erase(conn->name());
    EventLoop *ioloop = conn->getLoop();
    ioloop->queueLoop(std::bind(&TcpConnection::connectionDestroyed, conn));
}