#pragma once

#include "Poller.h"
#include "Timestamp.h"
#include <vector>
#include <sys/epoll.h>

/*
epoll的使用
epoll_create
epoll_ctl add mod del
epoll_wait
*/
class Channel;

// EPollPoller类，使用epoll实现IO复用，继承Poller抽象基类
class EPollPoller : public Poller
{
public:
    EPollPoller(EventLoop *loop); // 构造函数，初始化epoll
    ~EPollPoller() override;      // 析构函数，释放资源

    // 重写基类Poller的抽象方法
    Timestamp poll(int timeoutMs, ChannelList *activeChannels) override; // 等待IO事件
    void updateChannel(Channel *channel) override; // 更新或添加一个Channel
    void removeChannel(Channel *channel) override; // 移除一个Channel

private:
    static const int kInitEventListSize = 16; // 初始事件列表大小

    // 填写活跃的链接到activeChannels
    void fillActiveChannel(int numEvents, ChannelList *activeChannels) const;

    // 更新channel通道的epoll事件
    void update(int operation, Channel *channel);

    using EventList = std::vector<epoll_event>; // epoll事件列表类型

    int epollfd_;      // epoll文件描述符
    EventList events_; // 存储epoll_wait返回的事件
};
