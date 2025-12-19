#include "minio_storage.h"
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <fstream>
#include <iostream>

// 构造函数，初始化 Minio/S3 客户端
MinioStorage::MinioStorage(const std::string& endpoint,
                           const std::string& access_key,
                           const std::string& secret_key,
                           const std::string& bucket)
    : bucket_(bucket)
{
    Aws::Client::ClientConfiguration config;
    config.endpointOverride = endpoint;         // 设置 MinIO 服务地址
    config.scheme = Aws::Http::Scheme::HTTP;    // 使用 HTTP 协议
    config.verifySSL = false;                   // 不校验证书（本地开发可用）
    config.region = "us-east-1";                // 区域随意，MinIO 忽略
    client_ = std::make_shared<Aws::S3::S3Client>(
        Aws::Auth::AWSCredentials(access_key, secret_key), // 访问密钥
        config,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        false
    );
}

// 上传本地文件到 Minio/S3
bool MinioStorage::upload(const std::string& localFile, const std::string& objectName)
{
    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(bucket_);         // 设置桶名
    request.SetKey(objectName);         // 设置对象名

    // 打开本地文件作为输入流
    auto input_data = Aws::MakeShared<Aws::FStream>(
        "PutObjectInputStream", localFile.c_str(), std::ios_base::in | std::ios_base::binary);

    if (!input_data->good()) return false; // 文件打开失败
    request.SetBody(input_data);           // 设置请求体

    auto outcome = client_->PutObject(request); // 执行上传
    return outcome.IsSuccess();                // 返回是否成功
}

// 从 Minio/S3 下载对象到本地文件
bool MinioStorage::download(const std::string& objectName, const std::string& localFile)
{
    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(bucket_);         // 设置桶名
    request.SetKey(objectName);         // 设置对象名

    auto outcome = client_->GetObject(request); // 执行下载
    if (!outcome.IsSuccess()) return false;     // 下载失败

    auto& stream = outcome.GetResult().GetBody(); // 获取对象数据流
    std::ofstream output(localFile, std::ios::binary); // 打开本地文件
    output << stream.rdbuf();                          // 写入文件
    return output.good();                              // 返回是否成功
}

// 从 Minio/S3 删除对象
bool MinioStorage::remove(const std::string& objectName)
{
    Aws::S3::Model::DeleteObjectRequest request;
    request.SetBucket(bucket_);         // 设置桶名
    request.SetKey(objectName);         // 设置对象名

    auto outcome = client_->DeleteObject(request); // 执行删除
    return outcome.IsSuccess();                   // 返回是否成功
}

// 列出 Minio/S3 桶中的所有对象
bool MinioStorage::listObjects(std::vector<std::string>& objects)
{
    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(bucket_);

    auto outcome = client_->ListObjectsV2(request);
    if (!outcome.IsSuccess()) return false;

    const auto& result = outcome.GetResult();
    for (const auto& obj : result.GetContents()) {
        objects.push_back(obj.GetKey());
    }
    return true;
}

// 新增：带前缀列举
bool MinioStorage::listObjectsWithPrefix(const std::string& prefix, std::vector<std::string>& objects)
{
    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(bucket_);
    request.SetPrefix(prefix);

    auto outcome = client_->ListObjectsV2(request);
    if (!outcome.IsSuccess()) return false;

    const auto& result = outcome.GetResult();
    for (const auto& obj : result.GetContents()) {
        objects.push_back(obj.GetKey());
    }
    return true;
}

// 新增：对象拷贝
bool MinioStorage::copyObject(const std::string& src, const std::string& dst)
{
    Aws::S3::Model::CopyObjectRequest request;
    request.SetBucket(bucket_);
    request.SetCopySource(bucket_ + "/" + src); // 源对象
    request.SetKey(dst);                        // 目标对象

    auto outcome = client_->CopyObject(request);
    return outcome.IsSuccess();
}

// 新增：生成预签名 URL
std::string MinioStorage::presignedUrl(const std::string& objectName, int expireSeconds) {
    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(bucket_);
    request.SetKey(objectName);

    Aws::Http::URI uri;
    auto outcome = client_->GeneratePresignedUrl(
        bucket_, objectName, Aws::Http::HttpMethod::HTTP_GET, expireSeconds);
    if (outcome.empty()) return "";
    return outcome;
}