#include "file_server_db.h"
#include "file_server_util.h"
#include <iostream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>

MYSQL *mysqlConnect_(const DbCfg &cfg)
{
    MYSQL *m = mysql_init(nullptr);
    if (!m)
        return nullptr;
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(m, cfg.host.c_str(), cfg.user.c_str(), cfg.pass.c_str(), cfg.db.c_str(), cfg.port, nullptr, 0))
    {
        std::cerr << "[MySQL] connect failed: " << mysql_error(m) << std::endl;
        mysql_close(m);
        return nullptr;
    }
    return m;
}

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

bool dbRegister_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &err)
{
    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
    {
        err = "DB connect failed";
        return false;
    }

    const auto salt = randomBytes_(16);
    std::vector<unsigned char> concat;
    concat.insert(concat.end(), salt.begin(), salt.end());
    concat.insert(concat.end(), password.begin(), password.end());
    const auto hash = sha256_(concat);

    const std::string u = escape_(m, username);
    const std::string sql =
        "INSERT INTO users(username, password_hash, salt) VALUES('" + u + "', UNHEX('" + toHex_(hash) + "'), UNHEX('" + toHex_(salt) + "'))";

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

bool dbLogin_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &tokenOut, std::string &err)
{
    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
    {
        err = "DB connect failed";
        return false;
    }

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

    const long long userId = std::stoll(row[0]);
    const std::string hashHex = row[1] ? row[1] : "";
    const std::string saltHex = row[2] ? row[2] : "";
    mysql_free_result(res);

    const auto salt = hexToBytes_(saltHex);
    const auto stored = hexToBytes_(hashHex);

    std::vector<unsigned char> concat;
    concat.insert(concat.end(), salt.begin(), salt.end());
    concat.insert(concat.end(), password.begin(), password.end());
    const auto calc = sha256_(concat);

    if (calc != stored)
    {
        mysql_close(m);
        err = "Invalid username or password";
        return false;
    }

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

bool dbCheckToken_(const DbCfg &cfg, const std::string &token)
{
    if (token.empty())
        return false;

    MYSQL *m = mysqlConnect_(cfg);
    if (!m)
        return false;

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