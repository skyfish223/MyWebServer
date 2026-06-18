#pragma once
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <cstddef>

class ThreadPool
{
public:
    explicit ThreadPool(std::size_t threadCount);
    ~ThreadPool();
    void submit(std::function<void()> task);

private:
    void workerLoop();

    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    bool stop_;
};