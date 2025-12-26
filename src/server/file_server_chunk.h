#pragma once
#include <string>

bool parseChunkExtra_(const std::string &extra,
                      std::string &uploadId,
                      uint64_t &index,
                      uint64_t &total,
                      uint64_t &fullSize,
                      uint64_t &chunkSize);

std::string chunkBaseDir_();
std::string mergedBaseDir_();
bool ensureDir_(const std::string &path);
bool fileExists_(const std::string &path);
uint64_t fileSize_(const std::string &path);
bool mergeParts_(const std::string &sessionDir,
                 const std::string &mergedPath,
                 uint64_t totalParts,
                 uint64_t fullSize);
void cleanupSession_(const std::string &sessionDir, const std::string &mergedPath, uint64_t totalParts);