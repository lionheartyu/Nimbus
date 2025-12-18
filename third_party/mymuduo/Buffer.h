#pragma once
#include <vector>
#include <string>
#include <algorithm>

/// A buffer class modeled after org.jboss.netty.buffer.ChannelBuffer
///
/// @code
/// +-------------------+------------------+------------------+
/// | prependable bytes |  readable bytes  |  writable bytes  |
/// |                   |     (CONTENT)    |                  |
/// +-------------------+------------------+------------------+
/// |                   |                  |                  |
/// 0      <=      readerIndex   <=   writerIndex    <=     size
/// @endcode
class Buffer
{
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;

    explicit Buffer()
        : buffer_(kCheapPrepend + kInitialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend)
    {
    }

    // 返回可读的字节数
    size_t readableBytes() const
    {
        return writerIndex_ - readerIndex_;
    }

    // 返回可写的字节数
    size_t writableBytes() const
    {
        return buffer_.size() - writerIndex_;
    }

    // 返回可读的索引下标
    size_t prependableBytes() const
    {
        return readerIndex_;
    }

    // 返回缓冲区中可读数据的起始地址
    const char *peek() const
    {
        return begin() + readerIndex_;
    }

    // 复位
    //  onMessage string <- buffer
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readerIndex_ += len; // 应用只读取了刻度缓冲区数据的一部分，就是len,还剩下readerIndex +=len-> writerIndex_
        }
        else // len == readableBytes()
        {
            retrieveAll();
        }
    }

    void retrieveAll()
    {
        readerIndex_ = writerIndex_ = kCheapPrepend;
    }

    // 把onMessage函数上报的buffer数据 转成string类型的数据返回
    std::string retrieveAllAsString()
    {
        return retrieveAsString(readableBytes()); // 应用可读取数据的长度
    }

    std::string retrieveAsString(size_t len)
    {
        std::string reslut(peek(), len);
        retrieve(len); // 上面一句把缓冲区中的可读的数据,已经读取出来了,这里肯定要对缓冲区进行复位操作
        return reslut;
    }

    // buffer_.size() - writerIndex  len
    void ensureWriteableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容函数
        }
    }

    // 把[data,data+len]内存上的数据,添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        ensureWriteableBytes(len);
        std::copy(data, data + len, beginWrite());
        writerIndex_ += len;
    }

    char *beginWrite()
    {
        return begin() + writerIndex_;
    }

    // 从fd上读取数据
    ssize_t readFd(int fd, int *saveErrno);

    // 通过fd发送数据   
    ssize_t writeFd(int fd, int *saveErrno);
private:
    char *begin()
    {
        return &*buffer_.begin(); // vector底层数字首元素的地址,也就是数组的起始地址
    }

    const char *begin() const
    {
        return &*buffer_.begin(); // vector底层数字首元素的地址,也就是数组的起始地址
    }

    void makeSpace(size_t len)
    {
        /*
        kCheapPrepend | reader | wirter |
        kCheapPrepend |             len            |
        */
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
            buffer_.resize(writerIndex_ + len);
        }
        else
        {
            size_t readable = readableBytes();
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};