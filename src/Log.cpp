#include "Log.h"

#include <fstream>
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <ctime>

using namespace std;

Log& Log::instance()
{
    static Log inst;
    return inst;
}

void Log::init(const string& path, bool async)
{
    async_ = async;
    file_.open(path, ios::app);
    if(!file_)
    {
        cerr << "无法打开日志： " << path << "\n";
        return;
    }
    if(async_)
    {
        stop_ = false;
        worker_ = thread([this] () { log_thread_loop(); });
    }
}

void Log::write(LogLevel level, const string& msg)
{
    string line = format_line(level, msg);
    if(async_) write_async(line);
    else write_sync(line);
}

Log::~Log()
{
    if(async_ && worker_.joinable())
    {
        {
            lock_guard<mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    if(file_.is_open()) file_.close();  
}

string Log::format_line(LogLevel level, const string& msg)
{
    time_t now = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    const char* tag = "INFO";
    switch(level)
    {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info: tag = "INFO"; break;
        case LogLevel::Warn: tag = "WARN"; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }
    return string(tbuf) + " [" + tag + "] " + msg;
}

void Log::write_async(const string& line)
{
    {
        lock_guard<mutex> lock(mtx_);
        queue_.push(line);
    }
    cv_.notify_one();
}

void Log::write_sync(const string& line)
{
    lock_guard<mutex> lock(mtx_);
    if(file_.is_open())
    {
        file_ << line << endl;
        file_.flush();
    }
}

void Log::log_thread_loop()
{
    while(true)
    {
        string line;
        {
            unique_lock<mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if(stop_ && queue_.empty()) return;
            line = move(queue_.front());
            queue_.pop();
        }
        if(file_.is_open())
        {
            file_ << line << endl;
            file_.flush();
        }
    }
}