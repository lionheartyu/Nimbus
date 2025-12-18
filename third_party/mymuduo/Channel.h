#pragma once
#include "noncopyable.h"
#include "Timestamp.h"
#include <functional>
#include<memory>

/*
  理清楚  EventLoop Channel Poller之间的关系  <== Reactor模型上对应多路事件分发器
  Channel理解为通道,封装了sockfd 和 其感兴趣的event,如EPOLLIN,EPOLLOUT事件
  还绑定了poller返回的具体事件
*/

class EventLoop;

class Channel : noncopyable
{
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(Timestamp)>;

    Channel(EventLoop *loop, int fd);
    ~Channel();

    //fd得到poller通知以后,处理事件的。
    void handleEvent(Timestamp receiveTime);

    //设置回调函数对象
    void setReadcallback(ReadEventCallback cb){readcallback_=std::move(cb);}
    void setWritecallback(EventCallback cb){writecallback_=std::move(cb);}
    void setClosecallback(EventCallback cb){closecallback_=std::move(cb);}
    void setErrorcallback(EventCallback cb){errorcallback_=std::move(cb);}

    //防止当channel被手动remove掉,channel还在执行回调操作
    void tie(const std::shared_ptr<void>&);

    int fd()const {return fd_;}
    int events()const{return events_;}
    int set_revents(int revt){revents_=revt;return 0;}
    

    //设置fd相应的事件状态
    void enableReading(){events_|=kReadEvent;update();}
    void disableReading(){events_ &=~kReadEvent;update();} 

    void enableWriting(){events_|=kWriteEvent;update();}
    void disableWriting(){events_ &=~kWriteEvent;update();} 

    void disableAll(){events_=kNoneEvent;update();}

    //返回fd当前的事件状态
    bool isNoneEvent()const {return events_ == kNoneEvent;}
    bool isWriting()const{return events_ & kWriteEvent;}
    bool isReading()const{return events_ & kReadEvent;}

    int index(){return index_;}
    void setindex(int idx){index_=idx;}

    EventLoop* ownerLoop(){return loop_;}
    void remove();

private:
    void update();
    void handleEventWithGuard(Timestamp receiveTime);

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop *loop_;//事件循环
    const int fd_;//poller监听的对象
    int events_;//注册fd感兴趣的事件
    int revents_;//poller返回具体发生的事件
    int index_;

      std::weak_ptr<void>tie_;
      bool tied_;

      //因为channel 通道里面能够获知fd最终发生的具体的事件revents,所以它负责调用具体事件的回调操作
      ReadEventCallback readcallback_;
      EventCallback writecallback_;
      EventCallback closecallback_;
      EventCallback errorcallback_;
};