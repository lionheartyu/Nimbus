#include "EventLoopThread.h"

// 整个源文件的作用就是把一个新线程和一个新loop一一对应
// thread_ ==> 调用回调 threadFunc ==> thread_.start() 开启线程

// 构造函数，初始化成员变量
EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                 const std::string &name)
    : loop_(nullptr),
      exiting_(false),
      thread_(std::bind(&EventLoopThread::threadFunc, this), name), // 线程入口函数绑定为threadFunc
      mutex_(),
      cond_(),
      callback_(cb)
{
}

// 析构函数，安全退出事件循环并回收线程
EventLoopThread::~EventLoopThread()
{
    exiting_ = true;
    if (loop_ != nullptr)
    {
        loop_->quit();    // 让事件循环退出
        thread_.join();   // 等待线程结束
    }
}

// 启动线程并获取新线程中的EventLoop指针
EventLoop *EventLoopThread::startloop()
{
    thread_.start(); // 启动底层新线程

    EventLoop *loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == nullptr)
        {
            cond_.wait(lock); // 等待线程中EventLoop创建完成
        }
        loop = loop_; // 获取新线程中的EventLoop指针
    }
    return loop;
}

// 下面这个方法是在单独的新线程里面运行的
void EventLoopThread::threadFunc()
{
    // “One loop per thread”（每个线程一个事件循环）
    EventLoop loop; // 创建了一个独立的EventLoop，和上面的线程是一一对应的，one loop per thread模型
    if (callback_)
    {
        callback_(&loop); // 如果有初始化回调则执行
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop; // 设置loop_指针，通知主线程
        cond_.notify_one();
    }

    loop.loop(); // 启动事件循环（阻塞，直到quit）

    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr; // 事件循环结束后清空指针
}