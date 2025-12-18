#pragma once

/*
noncopyable被继承以后，派生类对象可以正常进行构造和析构，
但是派生类对象无法进行拷贝构造和赋值操作
*/

class noncopyable
{
public:
    noncopyable(const noncopyable &) = delete; // 禁用拷贝构造函数，防止对象被拷贝
    noncopyable &operator=(const noncopyable) = delete; // 禁用赋值操作符，防止对象被赋值

protected:
    noncopyable() = default;  // 允许默认构造
    ~noncopyable() = default; // 允许默认析构
};