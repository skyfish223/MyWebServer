#pragma once

#include <string>

struct ServerConfig
{
    std::string web_root = "www";
    int port = 8080;
    int max_events = 64;
    std::size_t thread_pool_size = 4;
};

extern ServerConfig g_cfg;