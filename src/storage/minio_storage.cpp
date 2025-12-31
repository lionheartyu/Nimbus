#include "minio_storage.h"
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartCopyRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/CompletedPart.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <sstream>
// 工具函数：百分号编码 UTF-8 字节流
// S3 只允许部分字符，其它字符都需要编码
static std::string urlEncodeAllBytes_(const std::string& s)
{
    std::ostringstream os;
    for (unsigned char c : s)
    {
        // S3 允许的字符：A-Z a-z 0-9 - _ . ~ /  其它都要编码
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
        {
            os << c;
        }
        else
        {
            os << '%' << std::uppercase << std::setw(2) << std::setfill('0') << std::hex << (int)c;
        }
    }
    return os.str();
}

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

// 新增：对象拷贝（修复 CopySource 需要 URL encode）
// 支持大文件分片拷贝
bool MinioStorage::copyObject(const std::string& src, const std::string& dst)
{
    // 先获取源对象大小
    Aws::S3::Model::HeadObjectRequest headReq;
    headReq.SetBucket(bucket_);
    headReq.SetKey(src.c_str());
    auto headOut = client_->HeadObject(headReq);
    if (!headOut.IsSuccess()) {
        std::cerr << "[MinIO] HeadObject failed: " << src << " err=" << headOut.GetError().GetMessage() << std::endl;
        return false;
    }
    uint64_t objSize = headOut.GetResult().GetContentLength();

    // 阈值：1GB，超过就用分片拷贝
    const uint64_t kMultipartThreshold = 1024ull * 1024 * 1024;
    const uint64_t kPartSize = 100ull * 1024 * 1024; // 100MB

    if (objSize <= kMultipartThreshold) {
        // 普通 CopyObject
        std::string encodedKey = urlEncodeAllBytes_(src);
        std::string copySource = bucket_ + "/" + encodedKey;

        Aws::S3::Model::CopyObjectRequest request;
        request.SetBucket(bucket_);
        request.SetCopySource(copySource.c_str());
        request.SetKey(dst.c_str());

        auto outcome = client_->CopyObject(request);
        if (!outcome.IsSuccess())
        {
            std::cerr << "[MinIO] CopyObject failed:\n"
                      << "  src=" << src << "\n"
                      << "  dst=" << dst << "\n"
                      << "  copySource=" << copySource << "\n"
                      << "  err=" << outcome.GetError().GetMessage() << std::endl;
            return false;
        }
        return true;
    }

    // 分片拷贝
    std::string encodedKey = urlEncodeAllBytes_(src);
    std::string copySource = bucket_ + "/" + encodedKey;

    // 1. CreateMultipartUpload
    Aws::S3::Model::CreateMultipartUploadRequest createReq;
    createReq.SetBucket(bucket_);
    createReq.SetKey(dst.c_str());
    auto createOut = client_->CreateMultipartUpload(createReq);
    if (!createOut.IsSuccess()) {
        std::cerr << "[MinIO] CreateMultipartUpload failed: " << dst << " err=" << createOut.GetError().GetMessage() << std::endl;
        return false;
    }
    std::string uploadId = createOut.GetResult().GetUploadId();

    std::vector<Aws::S3::Model::CompletedPart> completedParts;
    int partNumber = 1;
    uint64_t offset = 0;
    while (offset < objSize) {
        uint64_t thisPartSize = std::min(kPartSize, objSize - offset);
        std::ostringstream range;
        range << "bytes=" << offset << "-" << (offset + thisPartSize - 1);

        Aws::S3::Model::UploadPartCopyRequest partReq;
        partReq.SetBucket(bucket_);
        partReq.SetKey(dst.c_str());
        partReq.SetCopySource(copySource.c_str());
        partReq.SetCopySourceRange(range.str().c_str());
        partReq.SetPartNumber(partNumber);
        partReq.SetUploadId(uploadId);

        auto partOut = client_->UploadPartCopy(partReq);
        if (!partOut.IsSuccess()) {
            std::cerr << "[MinIO] UploadPartCopy failed: part=" << partNumber
                      << " range=" << range.str()
                      << " err=" << partOut.GetError().GetMessage() << std::endl;
            // Abort
            Aws::S3::Model::AbortMultipartUploadRequest abortReq;
            abortReq.SetBucket(bucket_);
            abortReq.SetKey(dst.c_str());
            abortReq.SetUploadId(uploadId);
            client_->AbortMultipartUpload(abortReq);
            return false;
        }
        Aws::S3::Model::CompletedPart completed;
        completed.SetPartNumber(partNumber);
        completed.SetETag(partOut.GetResult().GetCopyPartResult().GetETag());
        completedParts.push_back(completed);

        offset += thisPartSize;
        ++partNumber;
    }

    // 3. CompleteMultipartUpload
    Aws::S3::Model::CompletedMultipartUpload completedUpload;
    completedUpload.SetParts(completedParts);

    Aws::S3::Model::CompleteMultipartUploadRequest completeReq;
    completeReq.SetBucket(bucket_);
    completeReq.SetKey(dst.c_str());
    completeReq.SetUploadId(uploadId);
    completeReq.SetMultipartUpload(completedUpload);

    auto completeOut = client_->CompleteMultipartUpload(completeReq);
    if (!completeOut.IsSuccess()) {
        std::cerr << "[MinIO] CompleteMultipartUpload failed: " << dst
                  << " err=" << completeOut.GetError().GetMessage() << std::endl;
        return false;
    }
    return true;
}

// 新增：生成预签名 URL
// @param objectName 对象名
// @param expireSeconds 过期秒数
// @return 预签名URL字符串，失败返回空
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

// 新增：上传空对象（用于创建空文件或空文件夹）
bool MinioStorage::uploadEmpty(const std::string& objectName)
{
    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(bucket_);
    request.SetKey(objectName);

    auto emptyStream = Aws::MakeShared<Aws::StringStream>("EmptyFolderStream");
    request.SetBody(emptyStream);

    auto outcome = client_->PutObject(request);
    return outcome.IsSuccess();
}