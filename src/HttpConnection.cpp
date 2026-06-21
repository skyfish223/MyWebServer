#include "HttpConnection.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "EpollHelper.h"
#include "ServerConfig.h"
#include "ThreadPool.h"
#include "Log.h"

#include <iostream>
#include <vector>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cerrno>

using namespace std;

queue<ReadyResponse> g_ready_queue;
mutex g_ready_mtx;
int g_notify_fd = -1;

void wake_main_thread()
{
    if(g_notify_fd < 0) return;
    uint64_t one = 1;
    write(g_notify_fd, &one, sizeof(one));
}

HttpConnection::HttpConnection(int fd) : fd_(fd), last_active_(time(nullptr)) {}

void closeConnection(int epfd, int fd,
                    unordered_map<int, HttpConnection>& conns)
{
    auto it = conns.find(fd);
    if(it == conns.end()) return;
    removeFd(epfd, fd);
    close(fd);
    conns.erase(it);
}

void tick_expired_connections(int epfd,
                            unordered_map<int, HttpConnection>& conns)
{
    vector<int> expired;

    for(const auto& [fd, conn] : conns)
    {
        if(conn.phase_ == ConnPhase::Reading && conn.is_expired(g_cfg.conn_timeout_sec))
            expired.push_back(fd);
    }

    for(int fd : expired)
    {
        //cout << "[main] 连接超时 fd=" << fd << "\n";
        LOG_WARN("超时 fd=" + to_string(fd));
        closeConnection(epfd, fd, conns);
    }
}

WriteResult process_write(int epfd, HttpConnection& conn)
{
    conn.refresh_active();

    while(conn.write_idx_ < conn.write_buf_.size())
    {
        size_t remaining = conn.write_buf_.size() - conn.write_idx_;
        ssize_t n = write(conn.fd_,
                        conn.write_buf_.data() + conn.write_idx_,
                        remaining);
        if(n > 0)
        {
            conn.write_idx_ += static_cast<size_t>(n);
            continue;
        }
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            modFd(epfd, conn.fd_, EPOLLOUT);
            return WriteResult::Incomplete;
        }
        return WriteResult::Error;
    }
    return WriteResult::Complete;
}

void handle_write(int epfd, HttpConnection& conn,
                unordered_map<int, HttpConnection>& conns)
{
    if(conn.phase_ != ConnPhase::Writing) return;

    WriteResult wr = process_write(epfd, conn);
    if(wr == WriteResult::Complete)
    {
        /*cout << "[main] 发完 fd=" << conn.fd_
             << " 共 " << conn.write_buf_.size() << " 字节\n";*/
        closeConnection(epfd, conn.fd_, conns);
    }
    else if(wr == WriteResult::Error)
    {
        //cout << "[main] 写失败 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
    }
}

void drain_ready_responses(int epfd, unordered_map<int, HttpConnection>& conns)
{
    queue<ReadyResponse> local;
    {
        lock_guard<mutex> lock(g_ready_mtx);
        swap(local, g_ready_queue);
    }
    while(!local.empty())
    {
        ReadyResponse rr = move(local.front());
        local.pop();

        auto it = conns.find(rr.fd);
        if(it == conns.end()) continue;

        HttpConnection& conn = it->second;
        if(conn.phase_ != ConnPhase::WaitingResponse) continue;

        conn.write_buf_ = move(rr.data);
        conn.write_idx_ = 0;
        conn.phase_ = ConnPhase::Writing;

        cout << "[main] 开始发送 fd=" << conn.fd_
             << " 响应长度=" << conn.write_buf_.size() << "\n";

        modFd(epfd, conn.fd_, EPOLLOUT);

        WriteResult wr = process_write(epfd, conn);
        if(wr == WriteResult::Complete)
        {
            cout << "[main] 发完 fd=" << conn.fd_ << "\n";
            closeConnection(epfd, conn.fd_, conns);
        }
        else if(wr == WriteResult::Error)
        {
            closeConnection(epfd, conn.fd_, conns);
        }
    }
}

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                unordered_map<int, HttpConnection>& conns)
{
    if(conn.phase_ != ConnPhase::Reading) return;

    if(!append_read(conn))
    {
        cout << "[main] 读失败或连接关闭 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
        return;
    }

    conn.refresh_active();

    ReadResult result = process_read(conn);

    if(result == ReadResult::Incomplete)
    {
        return;
    }
    if(result == ReadResult::Error)
    {
        cout << "[main] 解析错误 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
        return;
    }

    HttpRequest req = move(conn.request_);
    int client_fd = conn.fd_;
    conn.phase_ = ConnPhase::WaitingResponse;
    modFd(epfd, client_fd, 0);

    /*cout << "[main] 请求完整 fd=" << client_fd << " "
         << req.method << " " << req.path << "\n";*/

    LOG_INFO("请求完整 " + req.method + " " + req.path);

    pool.submit([client_fd, req]() {
        /*cout << "[worker " << this_thread::get_id() << "] "
            << req.method << " " << req.path;*/
            if(!req.body.empty()) cout << " body_len=" << req.body.size();
            cout << "\n";

            string response = do_request(req);
            {
                lock_guard<mutex> lock(g_ready_mtx);
                g_ready_queue.push({client_fd, move(response)});
            }
            wake_main_thread();
    });
}
