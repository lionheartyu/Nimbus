#pragma once
#include <string>
#include <mysql/mysql.h>

struct DbCfg
{
    std::string host;
    unsigned int port;
    std::string user;
    std::string pass;
    std::string db;
};

MYSQL *mysqlConnect_(const DbCfg &cfg);
std::string escape_(MYSQL *m, const std::string &s);
bool dbRegister_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &err);
bool dbLogin_(const DbCfg &cfg, const std::string &username, const std::string &password, std::string &tokenOut, std::string &err);
bool dbCheckToken_(const DbCfg &cfg, const std::string &token);