#include <iostream>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <cerrno>

#include "ServerConfig.h"
#include "EpollHelper.h"
#include "HttpConnection.h"
#include "ThreadPool.h"


using namespace std;

int main()
{
    ThreadPool pool(g_cfg.thread_pool_size);

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

    cout << "epoll 服务器已启动：http://127.0.0.1:" << g_cfg.port << endl;
    cout << "工作线程数: " << g_cfg.thread_pool_size << "\n";
    cout << "网站根目录: " << g_cfg.web_root << "/\n";

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
                    cout << "新连接 fd=" << client_fd << "\n";
                }
            }
            else
            {
                dispatchClient(epfd, fd, pool);
            }
        }
    }
    close(epfd);
    close(listen_fd);
    return 0;
}