#pragma once
#include <cstdint>

void setNonBlocking(int fd);
void addFd(int epfd, int fd, uint32_t events = 0x001); //0x001 本质是 epoll 事件宏 EPOLLIN: #define EPOLLIN 0x00000001
void modFd(int epfd, int fd, uint32_t events);
void removeFd(int epfd, int fd);