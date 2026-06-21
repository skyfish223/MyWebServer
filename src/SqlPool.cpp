#include "SqlPool.h"
#include "Log.h"

#include <stdexcept>
#include <string>

using namespace std;

SqlPool& SqlPool::instance()
{
    static SqlPool p;
    return p;
}

void SqlPool::init(const char* host, int port, const char* user,
                const char* pass, const char* db, size_t poolSize)
{
    for(size_t i = 0; i < poolSize; i++)
    {
        MYSQL* conn = create_connection(host, port, user, pass, db);
        if(!conn)
        {
            throw runtime_error("MYSQL 连接池初始化失败");
        }
        pool_.push(conn);
    }
    LOG_INFO("MySQL 连接池就绪 size=" + to_string(poolSize));
}

MYSQL* SqlPool::get()
{
    unique_lock<mutex> lock(mtx_);
    cv_.wait(lock, [this] () { return !pool_.empty(); });
    MYSQL* conn = pool_.front();
    pool_.pop();
    return conn;
}

void SqlPool::release(MYSQL* conn)
{
    if(!conn) return;
    lock_guard<mutex> lock(mtx_);
    pool_.push(conn);
    cv_.notify_one();
}

MYSQL* SqlPool::create_connection(const char* host, int port,
                            const char* user, const char* pass, const char* db)
{
    MYSQL* conn = mysql_init(nullptr);
    if(!conn) return nullptr;
    if(!mysql_real_connect(conn, host, user, pass, db, port, nullptr, 0))
    {
        LOG_ERROR(string("mysql_real_connect: ") + mysql_error(conn));
        mysql_close(conn);
        return nullptr;
    }
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

ConnectionRAII::ConnectionRAII() : conn(SqlPool::instance().get()) {}

ConnectionRAII::~ConnectionRAII()
{
    SqlPool::instance().release(conn);
}