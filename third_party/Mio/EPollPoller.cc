#include "EPollPoller.h"
#include "logger.h"
#include "Channel.h"
#include "Timestamp.h"
#include <errno.h>
#include <unistd.h>
#include <string>
#include <string.h>
#include<strings.h>

// channel 未添加到poller中
const int kNew = -1; // channel中的成员index_ = -1
// channel 已添加到poller中
const int kAdded = 1;
// channel 从poller中删除
const int kDeleted = 2;

// 构造函数，创建epoll实例和初始化事件列表
const int kInitEventListSize = 16; // 确保大于0
EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) // vector<epoll_event>
{
    if (epollfd_ < 0)
    {
        LOG_FATAL("epoll_create error:%d\n", errno);
    }
}

// 析构函数，关闭epoll文件描述符
EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

// 等待IO事件发生，填充活跃的Channel到activeChannels
Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    // 实际上用LOG_DEBUG更为合适
    LOG_INFO("func=%s , fd total count %ld\n", __FUNCTION__, channels_.size());

    // 调用epoll_wait等待事件
    int numEvents = ::epoll_wait(epollfd_,&*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    // static_cast<int>(...)==>把 events_.size() 的返回值从 size_t 类型转换为 int 类型。

    int saveErrno = errno;

    Timestamp now(Timestamp::now());

    if (numEvents > 0)
    {
        LOG_INFO("%d events happened\n", numEvents);
        fillActiveChannel(numEvents, activeChannels); // 填充活跃通道
        if (numEvents == static_cast<int>(events_.size()))
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        LOG_DEBUG("%dtimeout\n", timeoutMs); // 超时无事件
    }
    else
    {
        if (saveErrno != EINTR)
        {
            errno = saveErrno;
            LOG_ERROR("EPOllPOller::poll()err!"); // 错误处理
        }
    }
    return now;
}

// channel update remove ==> eventloop updatechannel removechannel ==>poller updatechannel removechannel
/*
    EventLoop
    ChannelList   Poller
                  channelMap <fd,channel*>
*/

// 更新或添加一个Channel到epoll
void EPollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();
    LOG_INFO("func=%s fd=%d event=%d index=%d\n", __FUNCTION__, channel->fd(), channel->events(), channel->index());
    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel; // 添加到channels_映射表
        }
        else
        {
            // index = kDeleted
        }
        channel->setindex(kAdded); // 设置为已添加
        update(EPOLL_CTL_ADD, channel); // 添加到epoll
    }
    else // channel 以及在poller上注册过了
    {
        int fd = channel->fd();
        if (channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel); // 删除无事件的channel
            channel->setindex(kDeleted);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel); // 修改channel关注的事件
        }
    }
}

// 从epoll和channels_中移除一个Channel
void EPollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    channels_.erase(fd); // 从channels_映射表移除

    LOG_INFO("func=%s fd=%d \n", __FUNCTION__, fd);

    int index = channel->index();
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel); // 从epoll移除
    }
    channel->setindex(kDeleted); // 标记为已删除
}

// 填写活跃的链接到activeChannels
void EPollPoller::fillActiveChannel(int numEvents, ChannelList *activeChannels) const
{
    for (int i = 0; i < numEvents; i++)
    {
        Channel *channel = static_cast<Channel*>(events_[i].data.ptr); // 获取事件对应的Channel
        channel->set_revents(events_[i].events); // 设置发生的事件类型
        activeChannels->push_back(channel);//EventLoop就拿到了它的poller给它返回的所有发生事件的channel列表了
    }
}

// 更新channel通道的epoll事件
void EPollPoller::update(int operation, Channel *channel)
{
    epoll_event event;
    // memset(&event, 0, sizeof(event));
    bzero(&event,sizeof(event)); // 清空event结构体
    int fd = channel->fd();
    event.events = channel->events(); // 设置关注的事件
    event.data.fd = fd;
    event.data.ptr = channel; // 保存channel指针

    // 调用epoll_ctl进行操作
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR("epoll_ctl_del error:%d\n", errno);
        }
        else
        {
            LOG_FATAL("epoll_ctl_add/mod fatal:%d\n", errno);
        }
    }
}