#include "ServerConfig.h"

ServerConfig g_cfg;

const char* DB_HOST = "127.0.0.1";
const int   DB_PORT = 3306;
const char* DB_USER = "root";
const char* DB_PASS = "123456";   // ← 改成你本机 MySQL 密码
const char* DB_NAME = "webserver";
const std::size_t DB_POOL_SIZE = 8;