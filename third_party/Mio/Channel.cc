#include "Channel.h"
#include "EventLoop.h"
#include"logger.h"
#include <sys/epoll.h>

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;//读事件水平触发
const int Channel::kWriteEvent = EPOLLOUT;//写事件

// EventLoop: ChannelList Poller

// 构造
Channel::Channel(EventLoop *loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(0),
      revents_(0),
      index_(-1),
      tied_(false)
{
}

// 析构
Channel::~Channel()
{
}

// Channel的tie方法什么时候调用过 一个TcpConnection新连接创建的时候 TcpConnection => Channel
void Channel::tie(const std::shared_ptr<void> &obj)
{
    tie_ = obj;
    tied_ = true;

    // 弱智能指针 转换为 强智能指针
}

/*
当改变channel所表示的events事件后，update负责在poller里面更改fd相应的事件epoll_ctl
EventLoop 包含 => ChannelList Poller
*/
void Channel::update()
{
    // 通过Channel所属的EventLoop调用poller的相应方法,注册fd的events事件

     loop_->updateChannel(this);
}

// 在Channel所属的EventLoop中,把当前的Channel删除掉
void Channel::remove()
{
    
     loop_->removeChannel(this);
}

// fd得到poller通知以后,处理事件的。
void Channel::handleEvent(Timestamp receiveTime)
{
    if (tied_)
    {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard)
        {
            handleEventWithGuard(receiveTime);
        }
    }
    else
    {
        handleEventWithGuard(receiveTime);
    }
}

//根据poller通知的channel发生的具体事件，由channel负责调用具体的回调操作  
void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    LOG_INFO("channel handleEvents revents:%d\n",revents_);

    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN))
    {
        if (closecallback_)
        {
            closecallback_();
        }
    }

    if (revents_ & EPOLLERR)
    {
        if (errorcallback_)
        {
            errorcallback_();
        }
    }

    if (revents_ & (EPOLLIN | EPOLLPRI))
    {
        if (readcallback_)
        {
            readcallback_(receiveTime);
        }
    }

    if (revents_ & EPOLLOUT)
    {
        if (writecallback_)
        {
            writecallback_();
        }
    }
}

// //test
// int main(){

// }