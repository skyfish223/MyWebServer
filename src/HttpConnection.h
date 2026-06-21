#pragma once

#include "HttpTypes.h"
#include <ctime>
#include <string>
#include <unordered_map>
#include <queue>
#include <mutex>

class ThreadPool;

class HttpConnection
{
public:
    explicit HttpConnection(int fd);

    int fd_;
    ParseState state_ = ParseState::RequestLine;
    std::string read_buf_;
    HttpRequest request_;

    ConnPhase phase_ = ConnPhase::Reading;
    std::string write_buf_;
    std::size_t write_idx_ = 0;

    time_t last_active_;

    void refresh_active() { last_active_ = time(nullptr); }

    bool is_expired(int timeout_sec) const
    {
        return difftime(time(nullptr), last_active_) >= timeout_sec;
    }
};

extern std::queue<ReadyResponse> g_ready_queue;
extern std::mutex g_ready_mtx;

extern int g_notify_fd;
void wake_main_thread();

void closeConnection(int epfd, int fd, std::unordered_map<int, HttpConnection>& conns);

void tick_expired_connections(int epfd, std::unordered_map<int, HttpConnection>& conns);

WriteResult process_write(int epfd, HttpConnection& conn);

void handle_write(int epfd, HttpConnection& conn,
                    std::unordered_map<int, HttpConnection>& conns);

void drain_ready_responses(int epfd, 
                        std::unordered_map<int, HttpConnection>& conns);

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                std::unordered_map<int, HttpConnection>& connections);
