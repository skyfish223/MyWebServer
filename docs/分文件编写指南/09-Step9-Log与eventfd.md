# 分文件 Step9 —— Log 与 eventfd 唤醒（详细版）

> **写给谁看：** 已完成 [分文件 Step8](./08-Step8-HttpConnection分次写.md)（EPOLLOUT 分次写 + `g_ready_queue` + 三阶段 `ConnPhase`）。  
> **对应单文件：** [step9.cpp](../../step9.cpp) · [09-日志系统指南](../09-日志系统指南.md)  
> **本篇目标：** 用 **`LOG_INFO` / `LOG_WARN` / `LOG_ERROR`** 替代零散 `cout`，日志写入 **`logs/server.log`**；默认 **异步写盘**，不拖慢 epoll 主线程。  
> **顺带修复 Step8：** 工作线程拼好响应后，用 **`eventfd`** 唤醒主线程 `drain`，解决浏览器一直转圈的问题。  
> **本步新增：** `Log.h` / `Log.cpp`；**修改** `HttpConnection`、`ServerConfig`、`main.cpp`、`Makefile`

---

## 目录

1. [本篇在架构中的位置](#一本篇在架构中的位置)
2. [本步结束后的文件树](#二本步结束后的文件树)
3. [第一部分：先搞懂「我们在升级什么」](#第一部分先搞懂我们在升级什么)
4. [第二部分：需要哪些新工具](#第二部分需要哪些新工具)
5. [第三部分：完整代码（可直接复制）](#第三部分完整代码可直接复制)
6. [第四部分：逐文件 / 逐块讲解](#第四部分逐文件--逐块讲解)
7. [第五部分：更新 Makefile](#第五部分更新-makefile)
8. [第六部分：编译、运行与验收](#第六部分编译运行与验收)
9. [常见问题排查](#常见问题排查)
10. [与单文件 step9.cpp / step11.cpp 对照](#与单文件-step9cpp--step11cpp-对照)
11. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

---

## 一、本篇在架构中的位置

```text
Step8：drain 只在 epoll 事件后 / 循环末尾执行 → 可能睡死
         ↓
Step9：worker push → wake_main_thread() → eventfd 可读
       epoll 唤醒 → drain → process_write → 页面正常
       同时：Log 异步写 logs/server.log
         ↓
Step10：+ SqlPool + 注册登录（Log 已就绪，SQL 错误可 LOG_WARN）
```

---

## 二、本步结束后的文件树

```text
MyWebServer/
├── src/
│   ├── Log.h                    ← Step9 新增
│   ├── Log.cpp                  ← Step9 新增
│   ├── ServerConfig.h           ← log_path、log_async
│   ├── HttpConnection.h         ← g_notify_fd、wake、drain 声明
│   ├── HttpConnection.cpp       ← eventfd 相关 + LOG 替换 cout
│   ├── main.cpp                 ← Log::init、eventfd 分支
│   └── ...（其余 Step8 文件保留）
├── logs/
│   └── server.log               ← 运行后生成
├── Makefile                     ← SRCS += Log.cpp
└── www/
```

---

## 第一部分：先搞懂「我们在升级什么」

### 1.1 Step8 的两个「工程债」

#### 债 1：日志只有 `cout`

| 问题 | 说明 |
|------|------|
| 终端刷屏 | 高并发时看不清 |
| 不能留档 | 关掉终端日志就没了 |
| 无级别 | 错误和普通信息混在一起 |
| 写盘慢 | 若在 epoll 线程里 `fprintf` 到文件，会拖慢所有连接 |

#### 债 2：浏览器可能一直转圈（Step8 典型 bug）

Step8 流程：

```text
主线程：请求完整 → modFd(0) → submit 线程池
工作线程：do_request → push g_ready_queue
主线程：epoll_wait(-1) 阻塞……
```

若工作线程 **在 `drain_ready_responses` 之后** 才把响应放进队列，主线程可能一直睡在 `epoll_wait` 里，**没人唤醒去发送** → 浏览器转圈。

Step9 用 **`eventfd`**：工作线程 `push` 后写 eventfd → epoll 唤醒 → `drain` → 页面正常。

### 1.2 生活类比

```text
cout     = 前台大声喊（所有人听见，忙时很吵）

同步日志 = 每来一位客人都亲自写账本（准确但慢）

异步日志 = 前台只往纸条盒扔纸条，专人后台抄账本
           （前台快，抄账本慢也不堵客人）

eventfd  = 工作线程按门铃（+1），前台 epoll 听见就来发货
```

### 1.3 本篇分两步理解（都在 Step9 完成）

| 阶段 | 配置 | 行为 |
|------|------|------|
| **Step9a 同步** | `g_cfg.log_async = false` | `LOG_INFO` 直接写 `logs/server.log` |
| **Step9b 异步（默认）** | `g_cfg.log_async = true` | 业务线程只入队，**单独 log 线程**写文件 |

先理解 9a（`Log.cpp` 里 `write_sync`），默认用 9b（`log_thread_loop`）。

### 1.4 本篇架构

```text
┌─────────────────────────────────────────────────────────────┐
│  main 线程（epoll）                                          │
│    EPOLLIN / EPOLLOUT / eventfd（Step9 新增）                 │
│    LOG_INFO("...")  → 异步队列 → log 线程写 server.log        │
│    fd == g_notify_fd → read 清空 → drain_ready_responses     │
└─────────────────────────────────────────────────────────────┘
         │                              │
         ▼                              ▼
   ThreadPool 工作线程              Log 后台线程
   do_request + push queue          从队列取行 → fwrite
   wake_main_thread()  ← Step9 新增
```

### 1.5 与 Step8 的区别

| 项目 | Step8 | Step9 |
|------|-------|-------|
| 诊断输出 | `cout` | **`LOG_*` + 文件** |
| 写日志 | 无 | 同步 / **异步** 可选 |
| 唤醒主线程 | 仅循环末尾 drain（有 bug 风险） | **`eventfd` + drain** |
| 新增模块 | — | **`Log.h/cpp`** |

### 1.6 对照 TinyWebServer

| 本篇 | Tiny |
|------|------|
| `Log` 单例 | `Log::get_instance()` |
| 阻塞队列 + log 线程 | `block_queue` + 异步写 |
| `LOG_INFO` 宏 | `LOG_INFO` / `LOG_ERROR` |
| `eventfd` 唤醒 | `socketpair` + 信号（思想相同） |

---

## 第二部分：需要哪些新工具

### 2.1 新增头文件

| 头文件 | 提供什么 | 用在哪 |
|--------|---------|--------|
| `<sys/eventfd.h>` | `eventfd()` | `HttpConnection.cpp`、`main.cpp` |
| `<fstream>` | `ofstream` | `Log.cpp` |
| `<ctime>` | `strftime` | `Log.cpp`（已有可复用） |

### 2.2 扩展 `ServerConfig`

```cpp
std::string log_path = "logs/server.log";
bool log_async = true;
```

Step11 还会加 `close_log`（压测关日志）；本步宏可先始终写日志，或提前预留。

### 2.3 `Log` 类设计

| 成员 / 方法 | 作用 |
|-------------|------|
| `instance()` | Meyers 单例 |
| `init(path, async)` | 打开文件；async 时启动 log 线程 |
| `write(level, msg)` | 格式化 + 同步或异步入队 |
| `format_line` | `2026-06-11 12:00:00 [INFO] ...` |
| `log_thread_loop` | 后台线程循环取队写盘 |

### 2.4 `eventfd` 唤醒主线程

| 步骤 | 谁做 |
|------|------|
| `main` 启动时 | `g_notify_fd = eventfd(0, EFD_NONBLOCK)`，`addFd(epfd, g_notify_fd)` |
| 工作线程 `push` 响应后 | `wake_main_thread()` |
| epoll 发现 `g_notify_fd` 可读 | `read` 清空计数 → `drain_ready_responses` |
| 循环末尾 | 仍可 `drain`（双保险） |

---

## 第三部分：完整代码（可直接复制）

以下 Log 与 eventfd 相关代码与 [step11.cpp](../../step11.cpp) 第 88～156、229～238、534～570、603～645 行一致（Step11 的 `g_cfg.close_log` 宏判断可在 [Step11](./11-Step11-ServerConfig与Makefile.md) 再加，本步先用简化宏）。

### 3.1 扩展 `src/ServerConfig.h`

```cpp
#pragma once
#include <string>

struct ServerConfig {
    std::string web_root = "www";
    std::string log_path = "logs/server.log";
    int port = 8080;
    std::size_t thread_pool_size = 4;
    int conn_timeout_sec = 15;
    bool log_async = true;          // Step9 新增
    std::size_t max_body_size = 1024 * 1024;
    int max_events = 64;
};

extern ServerConfig g_cfg;
```

---

### 3.2 完整 `src/Log.h`

```cpp
#pragma once

#include <string>

enum class LogLevel { Debug, Info, Warn, Error };

class Log {
public:
    static Log& instance();

    void init(const std::string& path, bool async);
    void write(LogLevel level, const std::string& msg);

    ~Log();

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

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

// Step9 简化宏（Step11 会加 g_cfg.close_log 判断）
#define LOG_INFO(msg)  Log::instance().write(LogLevel::Info,  (msg))
#define LOG_WARN(msg)  Log::instance().write(LogLevel::Warn,  (msg))
#define LOG_ERROR(msg) Log::instance().write(LogLevel::Error, (msg))
```

> `Log.h` 需在顶部 `#include <fstream>`、`<queue>`、`<mutex>`、`<thread>`、`<condition_variable>`，或改 Pimpl 隐藏实现细节。

---

### 3.3 完整 `src/Log.cpp`

```cpp
#include "Log.h"

#include <fstream>
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <ctime>

using namespace std;

Log& Log::instance() {
    static Log inst;
    return inst;
}

void Log::init(const string& path, bool async) {
    async_ = async;
    file_.open(path, ios::app);
    if (!file_) {
        cerr << "无法打开日志: " << path << "\n";
        return;
    }
    if (async_) {
        stop_ = false;
        worker_ = thread([this]() { log_thread_loop(); });
    }
}

void Log::write(LogLevel level, const string& msg) {
    string line = format_line(level, msg);
    if (async_) write_async(line);
    else write_sync(line);
}

Log::~Log() {
    if (async_ && worker_.joinable()) {
        {
            lock_guard<mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    if (file_.is_open()) file_.close();
}

string Log::format_line(LogLevel level, const string& msg) {
    time_t now = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    const char* tag = "INFO";
    switch (level) {
    case LogLevel::Debug: tag = "DEBUG"; break;
    case LogLevel::Info:  tag = "INFO";  break;
    case LogLevel::Warn:  tag = "WARN";  break;
    case LogLevel::Error: tag = "ERROR"; break;
    }
    return string(tbuf) + " [" + tag + "] " + msg;
}

void Log::write_sync(const string& line) {
    lock_guard<mutex> lock(mtx_);
    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
}

void Log::write_async(const string& line) {
    {
        lock_guard<mutex> lock(mtx_);
        queue_.push(line);
    }
    cv_.notify_one();
}

void Log::log_thread_loop() {
    while (true) {
        string line;
        {
            unique_lock<mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            line = move(queue_.front());
            queue_.pop();
        }
        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
        }
    }
}
```

---

### 3.4 完整 `src/HttpConnection.h`（Step9 增量）

在 Step8 基础上增加 eventfd 相关声明：

```cpp
#pragma once

#include "HttpTypes.h"
#include <ctime>
#include <string>
#include <queue>
#include <mutex>
#include <unordered_map>

class ThreadPool;

class HttpConnection {
public:
    explicit HttpConnection(int fd);

    int fd_;
    ParseState state_ = ParseState::RequestLine;
    std::string read_buf_;
    HttpRequest request_;
    time_t last_active_;

    ConnPhase phase_ = ConnPhase::Reading;
    std::string write_buf_;
    std::size_t write_idx_ = 0;

    void refresh_active() { last_active_ = time(nullptr); }

    bool is_expired(int timeout_sec) const {
        return difftime(time(nullptr), last_active_) >= timeout_sec;
    }
};

extern std::queue<ReadyResponse> g_ready_queue;
extern std::mutex g_ready_mtx;
extern int g_notify_fd;

void wake_main_thread();

void closeConnection(int epfd, int fd,
                     std::unordered_map<int, HttpConnection>& conns);

void tick_expired_connections(int epfd,
                              std::unordered_map<int, HttpConnection>& conns);

WriteResult process_write(int epfd, HttpConnection& conn);

void handle_write(int epfd, HttpConnection& conn,
                  std::unordered_map<int, HttpConnection>& conns);

void drain_ready_responses(int epfd,
                           std::unordered_map<int, HttpConnection>& conns);

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 std::unordered_map<int, HttpConnection>& conns);
```

---

### 3.5 完整 `src/HttpConnection.cpp`（Step9 版）

与 [step11.cpp](../../step11.cpp) 对齐，关键差异：`LOG_*` 替代超时/请求相关的 `cout`；worker 里 **`wake_main_thread()`**。

```cpp
#include "HttpConnection.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "EpollHelper.h"
#include "ServerConfig.h"
#include "ThreadPool.h"
#include "Log.h"

#include <vector>
#include <unistd.h>
#include <cerrno>
#include <sys/eventfd.h>

using namespace std;

queue<ReadyResponse> g_ready_queue;
mutex g_ready_mtx;
int g_notify_fd = -1;

void wake_main_thread() {
    if (g_notify_fd < 0) return;
    uint64_t one = 1;
    write(g_notify_fd, &one, sizeof(one));
}

HttpConnection::HttpConnection(int fd)
    : fd_(fd), last_active_(time(nullptr)) {}

void closeConnection(int epfd, int fd,
                     unordered_map<int, HttpConnection>& conns) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    removeFd(epfd, fd);
    close(fd);
    conns.erase(it);
}

void tick_expired_connections(int epfd,
                              unordered_map<int, HttpConnection>& conns) {
    vector<int> expired;
    for (const auto& [fd, conn] : conns) {
        if (conn.phase_ == ConnPhase::Reading &&
            conn.is_expired(g_cfg.conn_timeout_sec))
            expired.push_back(fd);
    }
    for (int fd : expired) {
        LOG_WARN("超时 fd=" + to_string(fd));
        closeConnection(epfd, fd, conns);
    }
}

WriteResult process_write(int epfd, HttpConnection& c) {
    c.refresh_active();
    while (c.write_idx_ < c.write_buf_.size()) {
        ssize_t n = write(c.fd_, c.write_buf_.data() + c.write_idx_,
                          c.write_buf_.size() - c.write_idx_);
        if (n > 0) {
            c.write_idx_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            modFd(epfd, c.fd_, EPOLLOUT);
            return WriteResult::Incomplete;
        }
        return WriteResult::Error;
    }
    return WriteResult::Complete;
}

void handle_write(int epfd, HttpConnection& c,
                  unordered_map<int, HttpConnection>& conns) {
    if (c.phase_ != ConnPhase::Writing) return;
    WriteResult w = process_write(epfd, c);
    if (w == WriteResult::Complete || w == WriteResult::Error)
        closeConnection(epfd, c.fd_, conns);
}

void drain_ready_responses(int epfd,
                           unordered_map<int, HttpConnection>& conns) {
    queue<ReadyResponse> local;
    {
        lock_guard<mutex> lk(g_ready_mtx);
        swap(local, g_ready_queue);
    }
    while (!local.empty()) {
        auto rr = move(local.front());
        local.pop();
        auto it = conns.find(rr.fd);
        if (it == conns.end()) continue;
        HttpConnection& c = it->second;
        if (c.phase_ != ConnPhase::WaitingResponse) continue;
        c.write_buf_ = move(rr.data);
        c.write_idx_ = 0;
        c.phase_ = ConnPhase::Writing;
        modFd(epfd, c.fd_, EPOLLOUT);
        WriteResult w = process_write(epfd, c);
        if (w == WriteResult::Complete || w == WriteResult::Error)
            closeConnection(epfd, c.fd_, conns);
    }
}

void handle_read(int epfd, HttpConnection& c, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& conns) {
    if (c.phase_ != ConnPhase::Reading) return;
    if (!append_read(c)) {
        closeConnection(epfd, c.fd_, conns);
        return;
    }
    c.refresh_active();
    ReadResult r = process_read(c);
    if (r == ReadResult::Incomplete) return;
    if (r == ReadResult::Error) {
        closeConnection(epfd, c.fd_, conns);
        return;
    }
    HttpRequest req = move(c.request_);
    int fd = c.fd_;
    c.phase_ = ConnPhase::WaitingResponse;
    modFd(epfd, fd, 0);
    LOG_INFO("请求完整 " + req.method + " " + req.path);
    pool.submit([fd, req]() {
        string resp = do_request(req);
        {
            lock_guard<mutex> lk(g_ready_mtx);
            g_ready_queue.push({fd, move(resp)});
        }
        wake_main_thread();   // Step9 核心：唤醒 epoll 主线程
    });
}
```

---

### 3.6 完整 `src/main.cpp`（Step9 版）

```cpp
#include <iostream>
#include <unordered_map>
#include <vector>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>

#include "ServerConfig.h"
#include "EpollHelper.h"
#include "HttpConnection.h"
#include "ThreadPool.h"
#include "Log.h"

using namespace std;

int main() {
    Log::instance().init(g_cfg.log_path, g_cfg.log_async);

    ThreadPool pool(g_cfg.thread_pool_size);
    unordered_map<int, HttpConnection> connections;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_cfg.port);
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);
    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);
    addFd(epfd, listen_fd);

    g_notify_fd = eventfd(0, EFD_NONBLOCK);
    if (g_notify_fd < 0) {
        LOG_ERROR("eventfd 创建失败");
        return 1;
    }
    addFd(epfd, g_notify_fd);

    cout << "Step9 已启动 http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "日志文件: " << g_cfg.log_path
         << " 异步=" << (g_cfg.log_async ? "是" : "否") << "\n";
    LOG_INFO("Step9 服务器启动");

    vector<epoll_event> events(g_cfg.max_events);

    while (true) {
        int n = epoll_wait(epfd, events.data(), g_cfg.max_events, -1);
        if (n < 0) continue;

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            // ── Step9：eventfd 唤醒分支 ──
            if (fd == g_notify_fd) {
                uint64_t u;
                while (read(g_notify_fd, &u, sizeof(u)) > 0) {}
                drain_ready_responses(epfd, connections);
                continue;
            }

            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in ca{};
                    socklen_t len = sizeof(ca);
                    int cfd = accept(listen_fd, (sockaddr*)&ca, &len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    setNonBlocking(cfd);
                    addFd(epfd, cfd);
                    connections.emplace(cfd, HttpConnection(cfd));
                    LOG_INFO("新连接 fd=" + to_string(cfd));
                }
            } else {
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                if (ev & EPOLLIN)
                    handle_read(epfd, it->second, pool, connections);
                if (ev & EPOLLOUT) {
                    it = connections.find(fd);
                    if (it != connections.end())
                        handle_write(epfd, it->second, connections);
                }
            }
        }

        drain_ready_responses(epfd, connections);
        tick_expired_connections(epfd, connections);
    }

    return 0;
}
```

---

## 第四部分：逐文件 / 逐块讲解

### 4.1 Log 与 eventfd 要点

**Log：** `init` 以 `ios::app` 打开日志；`log_async=true` 时后台线程 `log_thread_loop` 写盘，业务线程只入队。析构 `stop_` + `join` 防丢日志。

**eventfd：**

```cpp
g_notify_fd = eventfd(0, EFD_NONBLOCK);
addFd(epfd, g_notify_fd);
```

| 参数 | 含义 |
|------|------|
| `0` | 初始计数 0 |
| `EFD_NONBLOCK` | `write` 不会阻塞 worker |

`g_notify_fd` 与普通 client fd **一样**挂进 epoll，统一在一个 `epoll_wait` 里处理。

### 4.2 `wake_main_thread` 与 drain 顺序

```cpp
uint64_t one = 1;
write(g_notify_fd, &one, sizeof(one));
```

- 计数 +1，epoll 报告 **`g_notify_fd` 可读**。
- 主线程 `read` 循环清空计数（可能多次 wake 累加）：

```cpp
while (read(g_notify_fd, &u, sizeof(u)) > 0) {}
```

- 然后 `drain_ready_responses` 发送响应。

### 4.5 为何 worker 里要 `{ lock; push; }` 再 `wake`？

```cpp
pool.submit([fd, req]() {
    string resp = do_request(req);
    { lock_guard<mutex> lk(g_ready_mtx); g_ready_queue.push({fd, move(resp)}); }
    wake_main_thread();
});
```

- **先 push 再 wake**：避免主线程被唤醒时队列还是空的。
- **wake 在锁外**：`write(eventfd)` 不必持 `g_ready_mtx`，减少锁持有时间。

### 4.6 eventfd 分支为何要 `continue`？

```cpp
if (fd == g_notify_fd) {
    while (read(...) > 0) {}
    drain_ready_responses(...);
    continue;   // 不要当成 client fd 去 handle_read
}
```

`g_notify_fd` 不是 HTTP 连接，不能走 `handle_read` / `handle_write`。

### 4.7 循环末尾仍保留 `drain` 的原因

双路径保证响应发出：

1. **eventfd 唤醒** → 立即 drain（主路径，修 Step8 转圈）
2. **循环末尾 drain** → 若本轮已有其它 epoll 事件，也能顺带取队列

与 [step11.cpp](../../step11.cpp) 643～645 行一致。

### 4.8 部署前创建 `logs/` 目录

```bash
mkdir -p logs
```

若目录不存在，`file_.open` 失败，服务器仍可跑但没有文件日志。

---

## 第五部分：更新 Makefile

Step9 **新增** `Log.cpp`：

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -Isrc
LDFLAGS  = -pthread

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
       src/Log.cpp \
       src/ThreadPool.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp \
       src/HttpConnection.cpp \
       src/EpollHelper.cpp

server: $(SRCS)
	$(CXX) $(CXXFLAGS) -o server $(SRCS) $(LDFLAGS)

clean:
	rm -f server

.PHONY: clean server
```

> Step10 会再加 `SqlPool.cpp`、`FormParser.cpp` 和 `-lmysqlclient`；本步无需 MySQL。

---

## 第六部分：编译、运行与验收

### 6.1 编译与启动

```bash
mkdir -p logs
make clean && make
./server
```

### 6.2 功能验收

**浏览器（Step9 核心验收 —— 不应再转圈）：**

```text
打开 http://127.0.0.1:8080/
打开 http://127.0.0.1:8080/echo
提交 POST 表单
```

**命令行：**

```bash
curl http://127.0.0.1:8080/
curl -X POST http://127.0.0.1:8080/echo -d "a=b"
dd if=/dev/urandom of=www/big.bin bs=1M count=5
curl -o /tmp/big.bin http://127.0.0.1:8080/big.bin
cmp www/big.bin /tmp/big.bin
```

**日志文件：**

```bash
cat logs/server.log
```

应看到类似：

```text
2026-06-11 14:30:01 [INFO] Step9 服务器启动
2026-06-11 14:30:05 [INFO] 新连接 fd=6
2026-06-11 14:30:05 [INFO] 请求完整 GET /
```

### 6.3 验收 Checklist

- [ ] `make` 通过，无 `undefined reference to Log::`
- [ ] `logs/server.log` 存在且有启动、连接、请求记录
- [ ] 浏览器访问 `/` **不再无限转圈**
- [ ] POST `/echo` 正常
- [ ] 大文件 `big.bin` 下载完整
- [ ] `g_notify_fd` 已创建并 `addFd` 到 epoll
- [ ] worker `push` 后调用 `wake_main_thread()`
- [ ] `main` 有 `fd == g_notify_fd` 分支且 `read` 清空计数
- [ ] 超时连接写 `LOG_WARN` 而非仅 cout
- [ ] Makefile 已包含 `src/Log.cpp`

---

## 常见问题排查

### Q1：浏览器仍然转圈

| 检查项 |
|--------|
| worker 是否调用 `wake_main_thread()` |
| `g_notify_fd` 是否 `addFd` 到 epoll |
| `main` 是否处理 `fd == g_notify_fd` |
| `read(g_notify_fd)` 是否清空所有累积计数 |
| `drain` 内 `phase_ == WaitingResponse` 是否满足 |

### Q2：`logs/server.log` 为空

| 原因 |
|------|
| 未 `mkdir logs` |
| 未调用 `Log::instance().init` |
| 路径错误（相对路径以运行目录为准，应在项目根 `./server`） |
| 异步线程尚未 flush（稍等或正常退出进程） |

### Q3：`eventfd 创建失败`

| 原因 |
|------|
| 内核过旧（极少） |
| fd 耗尽 — 查 `ulimit -n` |

### Q4～Q5：编译 / 退出问题

- `ofstream` 不完整类型 → `Log.h` 加 `#include <fstream>`
- 退出时段错误 → 确保 `Log` 析构正常 `join`（勿 `quick_exit`）

### Q6：eventfd 计数暴涨

多次 `wake` 未 `read` 会累积；主线程已用 `while (read > 0)` 一次清空，属正常。若只 `read` 一次 8 字节，高并发下可能漏 drain — **必须循环 read**。

### Q7：与 step11 宏行为不一致

Step11 才加 `g_cfg.close_log` 判断；本步用简化宏，到 [Step11](./11-Step11-ServerConfig与Makefile.md) 再改。

---

## 与单文件 step9.cpp / step11.cpp 对照

| 单文件 | 分文件 Step9 |
|--------|--------------|
| `Log` 类 + `LOG_*` | `Log.h/cpp` |
| `g_notify_fd`、`wake_main_thread` | `HttpConnection.cpp` |
| eventfd epoll 分支 | `main.cpp` |
| `LOG_PATH`、`LOG_ASYNC` | `ServerConfig` |

从 [step11.cpp](../../step11.cpp) 按上表「剪贴到对应文件」即可；Step10 MySQL 部分本步不加。

---

## 本篇小结与下一篇预告

| 模块 | Step9 职责 |
|------|-----------|
| `Log.h/cpp` | 单例、同步/异步写盘、`LOG_*` 宏 |
| `ServerConfig` | `log_path`、`log_async` |
| `HttpConnection` | `g_ready_queue` + **`wake_main_thread`** |
| `main` | `Log::init`、**eventfd** 注册与 epoll 分支 |

**Step9 完成后：**

- 诊断信息持久化到 `logs/server.log`，默认异步不堵 epoll。
- **eventfd** 保证工作线程备好货后主线程一定能发货，浏览器不再转圈。

**下一篇：** [10-Step10-SqlPool与注册登录](./10-Step10-SqlPool与注册登录.md) —— MySQL 连接池、表单解析、POST `/register` 与 `/login`；`LOG_WARN` 记录 SQL 失败；Step9 的日志 / eventfd / 分次写 **全部保留**。
