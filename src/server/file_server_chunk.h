#pragma once
#include <string>

/// 解析分片上传 extra 字符串，提取 uploadId、index、total、fullSize、chunkSize
/// @param extra     分片上传 extra 字符串
/// @param uploadId  [输出] 上传会话ID
/// @param index     [输出] 当前分片序号
/// @param total     [输出] 分片总数
/// @param fullSize  [输出] 合并后完整文件大小
/// @param chunkSize [输出] 当前分片大小
/// @return 解析成功返回 true，否则 false
bool parseChunkExtra_(const std::string &extra,
                      std::string &uploadId,
                      uint64_t &index,
                      uint64_t &total,
                      uint64_t &fullSize,
                      uint64_t &chunkSize);

/// 分片临时存储目录
std::string chunkBaseDir_();

/// 合并后文件存储目录
std::string mergedBaseDir_();

/// 确保目录存在（递归创建）
/// @param path 目录路径
/// @return 创建成功或已存在返回 true，否则 false
bool ensureDir_(const std::string &path);

/// 判断文件是否存在
/// @param path 文件路径
/// @return 存在返回 true，否则 false
bool fileExists_(const std::string &path);

/// 获取文件大小（字节），失败返回0
/// @param path 文件路径
uint64_t fileSize_(const std::string &path);

/// 合并所有分片为一个完整文件
/// @param sessionDir 分片目录
/// @param mergedPath 合并后文件路径
/// @param totalParts 分片总数
/// @param fullSize   合并后文件应有大小（用于校验）
/// @return 合并成功返回 true，否则 false
bool mergeParts_(const std::string &sessionDir,
                 const std::string &mergedPath,
                 uint64_t totalParts,
                 uint64_t fullSize);

/// 清理分片会话目录和合并文件
/// @param sessionDir 分片目录
/// @param mergedPath 合并后文件路径
/// @param totalParts 分片总数
void cleanupSession_(const std::string &sessionDir, const std::string &mergedPath, uint64_t totalParts);