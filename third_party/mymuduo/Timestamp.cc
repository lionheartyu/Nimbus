#include "Timestamp.h"
#include <time.h>
#include<iostream>

Timestamp::Timestamp() : microSecondsSinceEpoch_(0)
{
}
Timestamp::Timestamp(int64_t microSecondsSinceEpoch) : microSecondsSinceEpoch_(microSecondsSinceEpoch)
{
}
Timestamp Timestamp::now()
{
    return Timestamp(time(NULL));
}
std::string Timestamp::toString() const
{
    char buf[128] = {0}; // 定义并初始化一个字符数组用于存储格式化后的时间字符串
    tm *tm_time = localtime(&microSecondsSinceEpoch_); // 将微秒时间戳转换为本地时间的tm结构体
    snprintf(buf, 128, "%4d/%02d/%02d %02d:%02d:%02d", // 按指定格式将时间写入buf
             tm_time->tm_year + 1900, // 年份需要加1900
             tm_time->tm_mon + 1,     // 月份范围为0-11，需要加1
             tm_time->tm_mday,        // 日
             tm_time->tm_hour,        // 时
             tm_time->tm_min,         // 分
             tm_time->tm_sec);        // 秒
    return buf; // 返回格式化后的字符串
}

// int main()
// {
//     // std::cout<<Timestamp::now().toString()<<std::endl;
//     return 0;
// }