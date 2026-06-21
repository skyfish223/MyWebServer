#include <iostream>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <cerrno>
#include <sys/eventfd.h>

#include "ServerConfig.h"
#include "EpollHelper.h"
#include "HttpConnection.h"
#include "ThreadPool.h"
#include "Log.h"
#include "SqlPool.h"

using namespace std;

int main()
{
    Log::instance().init(g_cfg.log_path, g_cfg.log_async);

    try {
        SqlPool::instance().init(DB_HOST, DB_PORT, DB_USER,
            DB_PASS, DB_NAME, DB_POOL_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "MySQL 初始化失败: " << e.what() << "\n";
        return 1;
    }

    ThreadPool pool(g_cfg.thread_pool_size);
    unordered_map<int, HttpConnection> connections;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0)
    {
        cerr << "socket 创建失败\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(g_cfg.port));

    if(bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        cerr << "bind 失败（端口可能被占用）\n";
        return 1;
    }

    if(listen(listen_fd, 128) < 0)
    {
        cerr << "listen 失败\n";
        return 1;
    }

    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);
    if(epfd < 0)
    {
        cerr << "epoll_create1 失败\n";
        return 1;
    }

    addFd(epfd, listen_fd);

    g_notify_fd = eventfd(0, EFD_NONBLOCK);
    if(g_notify_fd < 0)
    {
        LOG_ERROR("eventfd 创建失败");
        return 1;
    }
    addFd(epfd, g_notify_fd);

    cout << "Step7 定时器服务器已启动：http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "工作线程数: " << g_cfg.thread_pool_size << "\n";
    cout << "连接超时: " << g_cfg.conn_timeout_sec << " 秒\n";
    cout << "网站根目录: " << g_cfg.web_root << "/\n";
    cout << "超时测试: nc 127.0.0.1 " << g_cfg.port
         << "（连上后不发数据，等 " << g_cfg.conn_timeout_sec << " 秒）\n";
    cout << "大文件测试: 在 www/ 放 big.bin 后 curl -O http://127.0.0.1:"
         << g_cfg.port << "/big.bin\n";
    cout << "日志文件: " << g_cfg.log_path
         << " 异步=" << (g_cfg.log_async ? "是" : "否") << "\n";

    vector<epoll_event> events(static_cast<size_t>(g_cfg.max_events));

    while(true)
    {
        int nready = epoll_wait(epfd, events.data(), g_cfg.max_events, -1);
        if(nready < 0)
        {
            cerr << "epoll_wait 失败\n";
            continue;
        }

        for(int i = 0; i < nready; ++i)
        {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if(fd == g_notify_fd)
            {
                uint64_t u;
                while(read(g_notify_fd, &u, sizeof(u)) > 0) {}
                drain_ready_responses(epfd, connections);
                continue;
            }

            if(fd == listen_fd)
            {
                while(true)
                {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
                    if(client_fd < 0)
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        cerr << "accept 失败\n";
                        break;
                    }
                    setNonBlocking(client_fd);
                    addFd(epfd, client_fd);
                    connections.emplace(client_fd, HttpConnection(client_fd));
                    //cout << "[main] 新连接 fd=" << client_fd << "\n";
                    LOG_INFO("新连接 fd=" + to_string(client_fd));
                }
            }
            else
            {
                auto it = connections.find(fd);
                if(it == connections.end()) continue;
                if(ev & EPOLLIN)
                {
                    handle_read(epfd, it->second, pool, connections);
                }
                if(ev & EPOLLOUT)
                {
                    it = connections.find(fd);
                    if(it != connections.end())
                    {
                        handle_write(epfd, it->second, connections);
                    }
                }
            }
        }
        drain_ready_responses(epfd, connections);
        tick_expired_connections(epfd, connections);
    }
    close(epfd);
    close(listen_fd);
    return 0;
}