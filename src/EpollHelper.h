#pragma once
#include <cstdint>

void setNonBlocking(int fd);
void addFd(int epfd, int fd, uint32_t events = 0x001); // 0x001 = EPOLLIN
void modFd(int epfd, int fd, uint32_t events);
void removeFd(int epfd, int fd);
