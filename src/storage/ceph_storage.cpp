#include "ceph_storage.h"
#include <fstream>
#include <vector>
#include <iostream>
#include <cstring>

// 构造函数：初始化 Ceph 集群连接
CephStorage::CephStorage(const std::string &ceph_conf, const std::string &user, const std::string &pool)
{
    // 创建集群句柄
    int ret = rados_create(&cluster_, user.c_str());
    if (ret < 0)
    {
        std::cerr << "Failed to create cluster handle: " << ret << std::endl;
        connected_ = false;
        return;
    }
    // 读取 Ceph 配置文件
    ret = rados_conf_read_file(cluster_, ceph_conf.c_str());
    if (ret < 0)
    {
        std::cerr << "Failed to read ceph conf: " << ret << std::endl;
        rados_shutdown(cluster_);
        connected_ = false;
        return;
    }
    // 连接到 Ceph 集群
    ret = rados_connect(cluster_);
    if (ret < 0)
    {
        std::cerr << "Failed to connect to ceph cluster: " << ret << std::endl;
        rados_shutdown(cluster_);
        connected_ = false;
        return;
    }
    // 这里用传入的 pool 参数
    ret = rados_ioctx_create(cluster_, pool.c_str(), &io_ctx_);
    if (ret < 0)
    {
        std::cerr << "Failed to create ioctx: " << ret << std::endl;
        rados_shutdown(cluster_);
        connected_ = false;
        return;
    }
    connected_ = true;
}

// 析构函数：关闭 Ceph 连接
CephStorage::~CephStorage()
{
    if (connected_)
    {
        rados_ioctx_destroy(io_ctx_);
        rados_shutdown(cluster_);
    }
}

// 上传本地文件到 Ceph
bool CephStorage::upload(const std::string &localFile, const std::string &objectName)
{
    if (!connected_)
        return false;
    std::ifstream infile(localFile, std::ios::binary);
    if (!infile)
        return false;
    // 读取整个文件到 buffer
    std::vector<char> buffer((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    // 写入 Ceph 对象存储
    int ret = rados_write_full(io_ctx_, objectName.c_str(), buffer.data(), buffer.size());
    return ret >= 0;
}

// 从 Ceph 下载对象到本地文件
bool CephStorage::download(const std::string &objectName, const std::string &localFile)
{
    if (!connected_)
        return false;
    uint64_t obj_size = 0;
    time_t mtime;
    // 获取对象大小
    int ret = rados_stat(io_ctx_, objectName.c_str(), &obj_size, &mtime);
    if (ret < 0)
        return false;
    std::vector<char> buffer(obj_size);
    // 读取对象内容到 buffer
    ret = rados_read(io_ctx_, objectName.c_str(), buffer.data(), obj_size, 0);
    if (ret < 0)
        return false;
    std::ofstream outfile(localFile, std::ios::binary);
    if (!outfile)
        return false;
    // 写入本地文件
    outfile.write(buffer.data(), buffer.size());
    return true;
}

// 删除 Ceph 对象
bool CephStorage::remove(const std::string &objectName)
{
    if (!connected_)
        return false;
    int ret = rados_remove(io_ctx_, objectName.c_str());
    return ret >= 0;
}