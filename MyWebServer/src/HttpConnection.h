#pragma once

class ThreadPool;

void dispatchClient(int epfd, int client_fd, ThreadPool& pool);