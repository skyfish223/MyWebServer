#include "EpollHelper.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void addFd(int epfd, int fd, uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void modFd(int epfd, int fd, uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

void removeFd(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}