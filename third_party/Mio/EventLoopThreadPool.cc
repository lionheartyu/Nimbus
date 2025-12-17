#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

EventLoopThreadPoll::EventLoopThreadPoll(EventLoop *baseloop, const std::string nameArg)
    : baseloop_(baseloop),
      name_(nameArg),
      started_(false),
      numThread_(0),
      next_(0)
{
}

EventLoopThreadPoll::~EventLoopThreadPoll()
{
}

void EventLoopThreadPoll::start(const ThreadInitCallback &cb)
{
    started_ = true;
    for (int i = 0; i < numThread_; i++)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof(buf), "%s%d", name_.c_str(), i);
        EventLoopThread *t = new EventLoopThread(cb, buf);
        thraed_.push_back(std::unique_ptr<EventLoopThread>(t)); // 智能指针装t 自动释放资源
        loops_.push_back(t->startloop());                       // 底层创建线程绑定一个新的eventloop,并返回该loop的地址
    }

    // 整个服务端只有一个线程,运行着baseloop
    if (numThread_ == 0 && cb)
    {
        cb(baseloop_);
    }
}
// 如果工作在多线程中,baseloop_默认以轮询的方式分配channel给subloop
EventLoop *EventLoopThreadPoll::getNextLoop()
{
    EventLoop *loop = baseloop_;
    if (!loops_.empty())//通过轮询获取下一个处理事件的loop
    {
        loop = loops_[next_];
        next_++;
        if (next_ >= loops_.size())
        {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop *> EventLoopThreadPoll::getAllLoops()
{
    if (loops_.empty())
    {
        return std::vector<EventLoop *>(1, baseloop_);
    }
    else
    {
        return loops_;
    }
}
