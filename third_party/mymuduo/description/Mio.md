# 1.架构

- `EventLoop`：事件循环，负责调度和分发事件。
- `Channel`：封装文件描述符及其事件。
- `Poller`：IO复用（如epoll），负责监听事件。
- `TcpServer/TcpClient`：对外提供TCP服务/客户端接口。
- `TcpConnection`：每个连接的抽象。
- `Buffer`：数据缓冲区。

```
┌─────────┐
│ client │
└────┬────┘
     │
     ▼
┌──────────────┐
│ mainReactor │  (主Reactor，负责监听新连接)
└────┬─────────┘
     │
     ▼
┌──────────┐
│ acceptor │  (接收新连接)
└────┬─────┘
     │
     ▼
┌──────────────┐
│ subReactor  │  (从Reactor，负责已连接fd的IO事件)
└────┬─────────┘
     │
     ▼
┌─────────────┐
│   read      │  (读取数据)
└────┬────────┘
     │
     ▼
┌──────────────┐
│ Thread Pool  │  (线程池，处理耗时任务)
└────┬─────────┘
     │
     ▼
┌──────────────┐
│ decode      │
├─────────────┤
│ compute     │  (业务处理流程)
├─────────────┤
│ encode      │
└────┬────────┘
     │
     ▼
┌─────────────┐
│   send      │  (发送数据)
└─────────────┘
```

* 项目采用**主从多Reactor多线程**模型，**MainReactor** 只负责监听/派发新连接，在 **MainReactor** 中通过 **Acceptor** 接收新连接并通过设计好的**轮询算法**派发给 **SubReactor**，**SubReactor** 负责此连接的读写事件。

* 调用 **TcpServer** 的 **start 函数**后，会**内部创建线程池**。**每个线程独立的运行一个事件循环**，即 **SubReactor**。MainReactor 从线程池中轮询获取 SubReactor 并派发给它新连接，处理读写事件的 SubReactor 个数一般和 CPU 核心数相等。使用主从 Reactor 模型有诸多优点：

1. 响应快，不必为单个同步事件所阻塞，虽然 Reactor 本身依然是同步的；
2. 可以最大程度避免复杂的多线程之间同步问题，并且避免多线程模型的切换；
3. 扩展性好，可以方便地增加 Reactor 实例个数充分利用 CPU 资源；
4. 复用性好，Reactor 模型本身与具体事件处理逻辑无耦关系，具有很高的复用性；

# 2.核心理念

* MIO 网络库的核心理念 **“one loop per thread”** 的意思是：

* > **每个线程只运行一个事件循环（EventLoop），每个事件循环只属于一个线程。**

- 每个 EventLoop 负责管理一个线程内的所有 IO 事件（如网络读写、定时器等）。
- 多线程服务器中，每个工作线程有自己的 EventLoop，互不干扰。
- 主线程通常只负责接收新连接，然后把连接分发到某个工作线程（EventLoop）处理。

* **源码体现:**

```c++
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
```

# 3.框架梳理

**reactor模型**在实际设计中大致是有以下几个部分：

- Event：事件
- Reactor：反应堆
- Demultiplex：多路事件分发器
- EventHandler：事件处理器
- handler 就是“处理某个事件的代码”。

在MIO中，其调用关系大致如下

- 将事件及其处理方法注册到reactor，**reactor中存储了连接套接字connfd以及其感兴趣的事件event**
- reactor向其所对应的**demultiplex去注册相应的connfd+事件**，启动反应堆
- 当**demultiplex检测到connfd上有事件发生**，就会返回相应事件
- **reactor根据事件去调用eventhandler处理程序**

```bash
Event
  │
  └─> 注册Event和Handler
          │
          └─> Reactor
                  │
                  └─> [事件集合] loop
                          │
                          └─> 向Epoll add/mod/del Event
                                  │
                                  └─> Demultiplex
                                          │
                                          └─> 启动反应堆
                                                  │
                                                  └─> [事件分发器] loop
                                                          │
                                                          └─> 开启事件循环 epoll_wait
                                                                  │
                                                                  └─> 返回发生事件的Event
                                                                          │
                                                                          └─> Reactor
                                                                                  │
                                                                                  └─> 调用Event对应的事件处理器EventHandler
                                                                                          │
                                                                                          └─> EventHandler
```

* 而上述的，是在一个reactor反应堆中所执行的大致流程，其在MIO代码中包含关系如下（椭圆圈起来的是类）：

* 可以看到，**EventLoop其实就是我们的subreactor**，其执行在一个**Thread**上，实现了**one loop per thread**的设计。

* 每个EventLoop中，我们可以看到有**一个Poller和很多的Channel**，**Poller在上图调用关系中，其实就是demultiplex（多路事件分发器），而Channel对应的就是event（事件）**

```bash
Thread
  │
  └── one loop per thread ──> EventLoop
                                 │
                                 ├── 包含 ──> Poller
                                 │              │
                                 │              └── 包含 ──> Channel_map (sockfd, Channel*)
                                 │                                 │
                                 │                                 └── 包含 ──> sockfd, events, revents, 事件回调
                                 │
                                 ├── active_channel (Channel*)
                                 └── wakeup_fd
```

* 现在，我们大致明白了MIO每个reactor的设计，但是作为一个支持高并发的网络库，单线程往往不是一个好的设计。

* MIO采用了和Nginx相似的操作，有一个main reactor通过accept组件负责处理新的客户端连接，并将它们分派给各个sub reactor，每个sub reactor则是负责一个连接的读写等工作。

```bash
MIO网络库
   │
   └─> main reactor
           │
           └─> accept组件（负责处理新的客户端连接）
                   │
                   └─> 分派给各个 sub reactor
                           │
                           └─> 每个 sub reactor 负责一个连接的读写等工作
```

这里值得一提的是main Reactor也有一个EventLoop 只是其Loop循环的事件是等待连接也就是当Accept触发，那么EventLoop::Loop监听的Poller中epoll_wait就会被唤醒，然后EventLoop把相应的Channel执行其回调事件。这里的回调主要是TcpServer在Accept定义好的newconnection事件，用来初始化Tcpconnection对象的。这个时候就是我们的subReactor也就是我们会通过轮询算法给Tcpconnection对象分配EventLoop对象，此时这个EventLoop对应的就是一个线程，这就是MIO库重要思想**one loop per thread**。

