#pragma once
#include <string>
#include <mysql/mysql.h>

/// 数据库配置结构体
struct DbCfg
{
    std::string host;   ///< 数据库主机地址
    unsigned int port;  ///< 数据库端口
    std::string user;   ///< 数据库用户名
    std::string pass;   ///< 数据库密码
    std::string db;     ///< 数据库名
};

/// 连接MySQL数据库，返回MYSQL指针，失败返回nullptr
MYSQL *mysqlConnect_(const DbCfg &cfg);

/// 对字符串进行MySQL转义，防止SQL注入
std::string escape_(MYSQL *m, const std::string &s);

/// 用户注册，写入users表，密码加盐SHA256哈希
/// @param cfg      数据库配置
/// @param username 用户名
/// @param password 密码
/// @param err      [输出] 错误信息
/// @return 注册成功返回true，否则false
bool dbRegister_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &err);

/// 用户登录，校验密码，生成并写入session token
/// @param cfg      数据库配置
/// @param username 用户名
/// @param password 密码
/// @param tokenOut [输出] 登录成功生成的token
/// @param err      [输出] 错误信息
/// @return 登录成功返回true，否则false
bool dbLogin_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &tokenOut, std::string &err);

/// 校验token是否有效（sessions表中存在）
/// @param cfg   数据库配置
/// @param token 待校验的token
/// @return 有效返回true，否则false
bool dbCheckToken_(const DbCfg &cfg, const std::string &token);