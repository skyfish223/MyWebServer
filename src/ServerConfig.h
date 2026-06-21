#pragma once

#include <string>
#include <cstddef>

struct ServerConfig
{
    std::string web_root = "www";
    int port = 8080;
    int max_events = 64;
    std::size_t thread_pool_size = 4;
    std::size_t max_body_size = 1024 * 1024;

    int conn_timeout_sec = 15;

    std::string log_path = "logs/server.log";
    bool log_async = true;
};

extern ServerConfig g_cfg;

extern const char* DB_HOST;
extern const int   DB_PORT;
extern const char* DB_USER;
extern const char* DB_PASS;
extern const char* DB_NAME;
extern const std::size_t DB_POOL_SIZE;