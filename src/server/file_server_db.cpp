#include "file_server_db.h"
#include "file_server_util.h"
#include <iostream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>

/// 连接MySQL数据库，返回MYSQL指针，失败返回nullptr
MYSQL *mysqlConnect_(const DbCfg &cfg)
{
    MYSQL *m = mysql_init(nullptr);
    if (!m)
        return nullptr;
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    // 连接数据库
    if (!mysql_real_connect(m, cfg.host.c_str(), cfg.user.c_str(), cfg.pass.c_str(), cfg.db.c_str(), cfg.port, nullptr, 0))
    {
        std::cerr << "[MySQL] connect failed: " << mysql_error(m) << std::endl;
        mysql_close(m);
        return nullptr;
    }
    return m;
}

/// 对字符串进行MySQL转义，防止SQL注入
std::string escape_(MYSQL *m, const std::string &s)
{
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long n = 0;
    if (!out.empty())
    {
        n = mysql_real_escape_string(
            m,
            &out[0],
            s.c_str(),
            static_cast<unsigned long>(s.size()));
    }
    out.resize(n);
    return out;
}

/// 用户注册，写入users表，密码加盐SHA256哈希
/// @param cfg      数据库配置
/// @param username 用户名
/// @param password 密码
/// @param err      [输出] 错误信息
/// @return 注册成功返回true，否则false
bool dbRegister_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &err)
{
    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
    {
        err = "DB connect failed";
        return false;
    }

    // 生成16字节随机盐
    const auto salt = randomBytes_(16);
    // 拼接salt+password
    std::vector<unsigned char> concat;
    concat.insert(concat.end(), salt.begin(), salt.end());
    concat.insert(concat.end(), password.begin(), password.end());
    // 计算SHA256哈希
    const auto hash = sha256_(concat);

    // 转义用户名
    const std::string u = escape_(m, username);
    // 构造插入SQL
    const std::string sql =
        "INSERT INTO users(username, password_hash, salt) VALUES('" + u + "', UNHEX('" + toHex_(hash) + "'), UNHEX('" + toHex_(salt) + "'))";

    // 执行插入
    if (mysql_query(m, sql.c_str()) != 0)
    {
        const unsigned int ec = mysql_errno(m);
        err = (ec == 1062) ? "Username exists" : mysql_error(m);
        mysql_close(m);
        return false;
    }

    mysql_close(m);
    return true;
}

/// 用户登录，校验密码，生成并写入session token
/// @param cfg      数据库配置
/// @param username 用户名
/// @param password 密码
/// @param tokenOut [输出] 登录成功生成的token
/// @param err      [输出] 错误信息
/// @return 登录成功返回true，否则false
bool dbLogin_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &tokenOut, std::string &err)
{
    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
    {
        err = "DB connect failed";
        return false;
    }

    // 查询用户信息
    const std::string u = escape_(m, username);
    const std::string q = "SELECT id, HEX(password_hash), HEX(salt) FROM users WHERE username='" + u + "' LIMIT 1";
    if (mysql_query(m, q.c_str()) != 0)
    {
        err = mysql_error(m);
        mysql_close(m);
        return false;
    }

    MYSQL_RES *res = mysql_store_result(m);
    if (!res)
    {
        err = mysql_error(m);
        mysql_close(m);
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row)
    {
        mysql_free_result(res);
        mysql_close(m);
        err = "Invalid username or password";
        return false;
    }

    // 取出用户ID、哈希、盐
    const long long userId = std::stoll(row[0]);
    const std::string hashHex = row[1] ? row[1] : "";
    const std::string saltHex = row[2] ? row[2] : "";
    mysql_free_result(res);

    // 还原salt和hash
    const auto salt = hexToBytes_(saltHex);
    const auto stored = hexToBytes_(hashHex);

    // 计算输入密码的哈希
    std::vector<unsigned char> concat;
    concat.insert(concat.end(), salt.begin(), salt.end());
    concat.insert(concat.end(), password.begin(), password.end());
    const auto calc = sha256_(concat);

    // 校验密码
    if (calc != stored)
    {
        mysql_close(m);
        err = "Invalid username or password";
        return false;
    }

    // 生成token并写入sessions表
    const std::string token = genUuid_();
    const std::string sql =
        "INSERT INTO sessions(token, user_id) VALUES('" + escape_(m, token) + "', " + std::to_string(userId) + ")";

    if (mysql_query(m, sql.c_str()) != 0)
    {
        err = mysql_error(m);
        mysql_close(m);
        return false;
    }

    mysql_close(m);
    tokenOut = token;
    return true;
}

/// 校验token是否有效（sessions表中存在）
/// @param cfg   数据库配置
/// @param token 待校验的token
/// @return 有效返回true，否则false
bool dbCheckToken_(const DbCfg &cfg, const std::string &token)
{
    if (token.empty())
        return false;

    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
        return false;

    // 查询sessions表
    const std::string t = escape_(m, token);
    const std::string q = "SELECT user_id FROM sessions WHERE token='" + t + "' LIMIT 1";
    if (mysql_query(m, q.c_str()) != 0)
    {
        mysql_close(m);
        return false;
    }

    MYSQL_RES *res = mysql_store_result(m);
    if (!res)
    {
        mysql_close(m);
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    const bool ok = (row != nullptr);

    mysql_free_result(res);
    mysql_close(m);
    return ok;
}

bool dbLogout_(const DbCfg &cfg, const std::string &token){
    if (token.empty())
        return false;

    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
        return false;

    // 删除sessions表中的token
    const std::string t = escape_(m, token);
    const std::string sql = "DELETE FROM sessions WHERE token='" + t + "'";

    if (mysql_query(m, sql.c_str()) != 0)
    {
        mysql_close(m);
        return false;
    }

    mysql_close(m);
    return true;
}