#pragma once

#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstddef>

class SqlPool
{
public:
    static SqlPool& instance();

    void init(const char* host, int port, const char* user,
            const char* pass, const char* db, std::size_t poolSize);
    
    MYSQL* get();
    void release(MYSQL* conn);

private:
    SqlPool() = default;

    MYSQL* create_connection(const char* host, int port,
                            const char* user, const char* pass, const char* db);

    std::queue<MYSQL*> pool_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

struct ConnectionRAII {
    MYSQL* conn;
    ConnectionRAII();
    ~ConnectionRAII();
};