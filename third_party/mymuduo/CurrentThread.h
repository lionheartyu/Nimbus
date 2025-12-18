#pragma once
#include <unistd.h>
#include <sys/syscall.h>
namespace CurrentThread
{
    extern __thread int t_cachedTid;
    void cacheTid();//获取当前线程ID的接口

    inline int tid()
    {
        if (__builtin_expect(t_cachedTid==0,0))
        {
            cacheTid();
        }
        return t_cachedTid;
    }

}