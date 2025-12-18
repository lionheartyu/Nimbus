#pragma once

#include "noncopyable.h"
#include "Timestamp.h"
#include <vector>
#include <unordered_map>

class Channel;
class EventLoop;

// Poller类是IO复用的抽象基类，定义统一接口，供不同IO复用机制（如epoll、poll等）实现
class Poller : noncopyable
{
public:
    using ChannelList = std::vector<Channel *>; // 活跃通道列表类型

    Poller(EventLoop *loop); // 构造函数，绑定所属事件循环
    virtual ~Poller() = default; // 虚析构函数，允许派生类正确析构

    // 给所有IO复用保留统一的接口
    // 等待IO事件，timeoutMs为超时时间，activeChannels返回有事件的通道
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    // 更新或添加一个Channel
    virtual void updateChannel(Channel *channel) = 0;
    // 移除一个Channel
    virtual void removeChannel(Channel *channel) = 0;

    // 判断参数channel是否在当前Poller当中
    bool hasChannel(Channel *channel) const;

    // EventLoop可以通过该接口获取默认的IO复用的具体实现
    static Poller *newDefaultPoller(EventLoop *loop);

protected:
    // map的key: sockfd，value: sockfd所属的channel通道类型
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_; // 存储所有关注的Channel

private:
    EventLoop *ownerLoop_; // 定义Poller所属的事件循环
};