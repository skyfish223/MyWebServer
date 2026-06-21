#pragma once

#include <string>
#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

enum class LogLevel
{
    Debug,
    Info,
    Warn,
    Error
};

class Log
{
public:
    static Log& instance();

    void init(const std::string& paht, bool async);
    void write(LogLevel level, const std::string& msg);

    ~Log();

    Log(const Log&) = delete;
    Log& operator = (const Log&) = delete;

private:
    Log() = default;

    std::string format_line(LogLevel level, const std::string& msg);
    void write_sync(const std::string& line);
    void write_async(const std::string& line);
    void log_thread_loop();

    std::ofstream file_;
    bool async_ = true;
    bool stop_ = false;
    std::queue<std::string> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
};

#define LOG_INFO(msg)  Log::instance().write(LogLevel::Info,  (msg))
#define LOG_WARN(msg)  Log::instance().write(LogLevel::Warn,  (msg))
#define LOG_ERROR(msg) Log::instance().write(LogLevel::Error, (msg))