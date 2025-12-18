#include "CurrentThread.h"

namespace CurrentThread 
{
    __thread int t_cachedTid = 0;
    void cacheTid()
    {
        if (t_cachedTid == 0)
        {
            // 通过linux系统调用获取当前线程id的taid值
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}
