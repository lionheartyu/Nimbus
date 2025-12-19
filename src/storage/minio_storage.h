#pragma once
#include <string>
#include <memory>
#include <vector>
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>

class MinioStorage {
public:
    MinioStorage(const std::string& endpoint,
                 const std::string& access_key,
                 const std::string& secret_key,
                 const std::string& bucket);

    bool upload(const std::string& localFile, const std::string& objectName);
    bool download(const std::string& objectName, const std::string& localFile);
    bool remove(const std::string& objectName);
    bool listObjects(std::vector<std::string>& objects);

    bool listObjectsWithPrefix(const std::string& prefix, std::vector<std::string>& objects);
    bool copyObject(const std::string& src, const std::string& dst);

private:
    std::string bucket_;
    std::shared_ptr<Aws::S3::S3Client> client_;
};