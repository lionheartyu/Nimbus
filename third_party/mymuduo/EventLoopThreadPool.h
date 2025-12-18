#pragma once
#include "noncopyable.h"
#include <functional>
#include <string.h>
#include <string>
#include <vector>
#include <memory>

class EventLoop;
class EventLoopThread;

class EventLoopThreadPoll : noncopyable
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    EventLoopThreadPoll(EventLoop *baseloop, const std::string nameArg);
    ~EventLoopThreadPoll();

    void setThreadNum(int numThreads) { numThread_ = numThreads; }

    void start(const ThreadInitCallback &cb = ThreadInitCallback());

    // 如果工作在多线程中,baseloop_默认以轮询的方式分配channel给subloop
    EventLoop *getNextLoop();

    std::vector<EventLoop *> getAllLoops();

    bool started() const { return started_; }
    const std::string &name() const { return name_; }
    
private:
    EventLoop *baseloop_;
    std::string name_;
    bool started_;
    int numThread_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> thraed_;
    std::vector<EventLoop *> loops_;
};