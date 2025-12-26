#pragma once
#include <string>
#include <memory>
#include <vector>
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/DateTime.h>

/// Minio/S3 存储操作封装类
class MinioStorage {
public:
    /// 构造函数，初始化 Minio/S3 客户端
    /// @param endpoint   Minio/S3 服务地址（如 "127.0.0.1:9000"）
    /// @param access_key 访问密钥
    /// @param secret_key 密钥
    /// @param bucket     桶名
    MinioStorage(const std::string& endpoint,
                 const std::string& access_key,
                 const std::string& secret_key,
                 const std::string& bucket);

    /// 上传本地文件到 Minio/S3
    /// @param localFile  本地文件路径
    /// @param objectName 对象名（云端文件名）
    /// @return 上传成功返回 true
    bool upload(const std::string& localFile, const std::string& objectName);

    /// 从 Minio/S3 下载对象到本地文件
    /// @param objectName 对象名
    /// @param localFile  本地文件路径
    /// @return 下载成功返回 true
    bool download(const std::string& objectName, const std::string& localFile);

    /// 从 Minio/S3 删除对象
    /// @param objectName 对象名
    /// @return 删除成功返回 true
    bool remove(const std::string& objectName);

    /// 列出桶中所有对象
    /// @param objects [输出] 对象名列表
    /// @return 成功返回 true
    bool listObjects(std::vector<std::string>& objects);

    /// 列出指定前缀的对象
    /// @param prefix  前缀
    /// @param objects [输出] 对象名列表
    /// @return 成功返回 true
    bool listObjectsWithPrefix(const std::string& prefix, std::vector<std::string>& objects);

    /// 拷贝对象（支持大文件分片拷贝）
    /// @param src 源对象名
    /// @param dst 目标对象名
    /// @return 拷贝成功返回 true
    bool copyObject(const std::string& src, const std::string& dst);

    /// 生成对象的预签名下载URL
    /// @param objectName    对象名
    /// @param expireSeconds 过期秒数（默认1小时）
    /// @return 预签名URL字符串，失败返回空
    std::string presignedUrl(const std::string& objectName, int expireSeconds = 3600);

private:
    std::string bucket_;                             ///< 桶名
    std::shared_ptr<Aws::S3::S3Client> client_;      ///< S3客户端
};