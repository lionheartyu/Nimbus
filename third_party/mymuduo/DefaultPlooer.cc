#include "Poller.h"
#include"EPollPoller.h"
#include <stdlib.h>

Poller *Poller::newDefaultPoller(EventLoop *loop)
{
    if (::getenv("MUDUO_USE_POOL"))
    {
        return nullptr; // 生成poll的实例 暂时返回nullptr
    }
    else
    {
        return new EPollPoller(loop);
        // return nullptr; // 生成epoll的实例 暂时返回nullptr
    }
}