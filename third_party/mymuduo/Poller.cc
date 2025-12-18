#include "Poller.h"
#include "Channel.h"

// 构造函数，初始化所属事件循环
Poller::Poller(EventLoop *loop)
    : ownerLoop_(loop)
{
}

// 判断指定channel是否在channels_映射表中
bool Poller::hasChannel(Channel *channel) const
{
    auto it = channels_.find(channel->fd()); // 查找channel的文件描述符
    return it != channels_.end() && it->second == channel; // 存在且指针相同则返回true
}
