#pragma once
#include <string>
#include <rados/librados.h> // C接口

class CephStorage
{
public:
    CephStorage(const std::string &ceph_conf, const std::string &user, const std::string &pool);
    ~CephStorage();

    // 上传本地文件到 Ceph
    bool upload(const std::string &localFile, const std::string &objectName);

    // 从 Ceph 下载对象到本地文件
    bool download(const std::string &objectName, const std::string &localFile);

    // 删除 Ceph 对象
    bool remove(const std::string &objectName);

private:
    rados_t cluster_;
    rados_ioctx_t io_ctx_;
    bool connected_ = false;
};