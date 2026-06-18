# 分文件 Step5 —— ThreadPool 线程池

> **写给谁看：** 已完成 [Step4 分文件版](./04-Step4-epoll与EpollHelper.md) 的学习者——epoll 多连接能正常返回静态文件。  
> **对应单文件：** [Step1to6/Step5.cpp](../../Step1to6/Step5.cpp) · [05-线程池指南](../05-线程池指南.md)  
> **本篇目标：** 主线程 `read`，工作线程 `parseRequestLine` + `handleRequest` + `write` + `close`。  
> **本步新增：** `ThreadPool.h` / `ThreadPool.cpp`；**修改 `main.cpp`、`HttpConnection`、`ServerConfig`**

---

## 目录

1. [第一部分：架构说明——我们在升级什么](#第一部分架构说明我们在升级什么)
2. [第二部分：需要哪些新工具](#第二部分需要哪些新工具)
3. [第三部分：完整文件树](#第三部分完整文件树)
4. [第四部分：完整源码（逐步粘贴）](#第四部分完整源码逐步粘贴)
5. [第五部分：Makefile](#第五部分makefile)
6. [第六部分：逐块讲解](#第六部分逐块讲解)
7. [第七部分：编译、运行与验收](#第七部分编译运行与验收)
8. [第八部分：常见问题排查](#第八部分常见问题排查)
9. [第九部分：与单文件 Step5.cpp 对照](#第九部分与单文件-step5cpp-对照)
10. [第十部分：本篇小结与下一篇预告](#第十部分本篇小结与下一篇预告)

---

## 第一部分：架构说明——我们在升级什么

### 1.1 Step4 的瓶颈

Step4 用 epoll 同时**监视**很多连接，但 `handleClient` 里仍在一个线程里做完所有事：

```text
epoll_wait 返回 client 可读
    → read
    → parseRequestLine
    → handleRequest（读 www 文件、拼 HTTP）  ← 可能较慢，阻塞主循环
    → write
    → close
```

| 问题 | 说明 |
|------|------|
| **业务阻塞 I/O 线程** | 读大文件、拼响应时，主循环不能及时 `epoll_wait` 处理其它 fd |
| **单核吃满业务** | 静态文件逻辑和 epoll 调度挤在**同一个线程** |
| **多核浪费** | CPU 有 4 核 8 核，进程却主要用 1 个核跑业务 |

对小网站、本机测试仍可用；要更好利用多核，常见做法是 **Reactor（epoll 线程）+ 线程池（业务线程）**。

### 1.2 生活类比

| 角色 | Step4 | Step5 |
|------|-------|-------|
| **前台（epoll 主线程）** | 既迎宾又下厨 | **只迎宾、收单子**（accept、read） |
| **后厨（线程池）** | 没有 | **多个厨师**并行做菜（解析、读文件、拼响应） |
| **传菜** | 前台自己端 | 厨师做好后 **write + close** |

### 1.3 本篇架构（Reactor + 线程池）

```text
┌─────────────────────────────────────────────────────────┐
│  主线程（I/O 线程）                                       │
│  epoll_wait → accept / read → removeFd → 投递任务       │
│  （不再自己读 www、拼大段响应）                            │
└───────────────────────────┬─────────────────────────────┘
                            │ 任务队列（mutex 保护）
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
   工作线程 1           工作线程 2           工作线程 N
   解析 + 读文件        解析 + 读文件        ...
   write + close        write + close
```

**分工原则（本篇）：**

| 谁做 | 做什么 |
|------|--------|
| **主线程** | `epoll_wait`、`accept`、`read` 请求、`removeFd`、**`pool.submit`** |
| **工作线程** | `parseRequestLine`、`handleRequest`（读磁盘）、`write`、`close` |

> 为什么 `write` 放工作线程？每个 `client_fd` 彼此独立，不同连接由不同线程 write 是常见、可学习的简化模型。`epoll_wait` 仍只在主线程调用。

### 1.4 分文件后的模块职责

| 模块 | 本步变化 |
|------|---------|
| `ThreadPool` | **新增**：任务队列 + 工作线程 |
| `HttpConnection` | `handleClient` 改为 `dispatchClient`：read 后 submit |
| `main.cpp` | 创建 `ThreadPool`，传给 `dispatchClient` |
| `ServerConfig` | 新增 `thread_pool_size` |
| 其它模块 | 与 Step4 相同 |

### 1.5 本篇程序整体流程

```text
  启动
    → 创建 ThreadPool（如 4 个工作线程）
    → socket / bind / listen / epoll（同 Step4）
    → while (true) {
          epoll_wait
          listen_fd 就绪 → accept 循环 → addFd(client)
          client_fd 就绪 → dispatchClient：
                             read 请求
                             removeFd（从 epoll 摘掉，主线程不再管这个 fd）
                             pool.submit( 工作线程处理 )
        }

  工作线程里：
    parseRequestLine → handleRequest → write → close
```

---

## 第二部分：需要哪些新工具

### 2.1 C++ 标准库头文件

| 头文件 | 提供什么 | 为什么需要 |
|--------|---------|-----------|
| `<thread>` | `std::thread` | 创建工作线程 |
| `<mutex>` | `std::mutex`、`std::lock_guard`、`std::unique_lock` | 保护任务队列 |
| `<condition_variable>` | `std::condition_variable` | 队列为空时工作线程睡眠，有任务时唤醒 |
| `<queue>` | `std::queue` | 存放待执行的 `std::function<void()>` |
| `<functional>` | `std::function` | 把 lambda 包装成可投递的任务 |

### 2.2 编译链接选项

线程池需要 **pthread** 支持：

```makefile
CXXFLAGS += -pthread
LDFLAGS  += -pthread
```

g++ 在 Linux 上通常自动链接 pthread，但显式写上更稳妥。

### 2.3 线程安全要点

| 共享资源 | 保护方式 |
|---------|---------|
| `tasks_` 任务队列 | `mtx_` 互斥锁 |
| `stop_` 停止标志 | 同一把锁 |
| `client_fd` | 主线程 read 后交给**一个**工作线程，不再被其它线程触碰 |

**本步不做的事：** 主线程 read 完就 `removeFd`，工作线程独占这个 fd 直到 close——不需要更复杂的 fd 状态机。

---

## 第三部分：完整文件树

在 Step4 基础上 **新增** `ThreadPool`，**修改** `HttpConnection`、`main`、`ServerConfig`、`Makefile`：

```text
MyWebServer/
├── Makefile                     ← 增加 ThreadPool.cpp、-pthread
├── src/
│   ├── main.cpp                 ← 修改：创建 ThreadPool
│   ├── ServerConfig.h           ← 扩展 thread_pool_size
│   ├── ServerConfig.cpp
│   ├── HttpTypes.h
│   ├── HttpParser.h / .cpp
│   ├── HttpResponse.h / .cpp
│   ├── HttpHandler.h / .cpp
│   ├── EpollHelper.h / .cpp
│   ├── HttpConnection.h         ← 修改：dispatchClient
│   ├── HttpConnection.cpp       ← 修改
│   ├── ThreadPool.h             ← 新增
│   └── ThreadPool.cpp           ← 新增
└── www/
```

---

## 第四部分：完整源码（逐步粘贴）

下面只列出**本步有变化**的文件完整源码。未列出的文件（`HttpTypes`、`HttpParser`、`HttpResponse`、`HttpHandler`、`EpollHelper`）与 [Step4 文档](./04-Step4-epoll与EpollHelper.md) **完全相同**，无需修改。

### 4.1 `src/ServerConfig.h`（扩展）

```cpp
#pragma once
#include <string>
#include <cstddef>

struct ServerConfig {
    std::string web_root = "www";
    int port = 8080;
    int max_events = 64;
    std::size_t thread_pool_size = 4;
};

extern ServerConfig g_cfg;
```

### 4.2 `src/ServerConfig.cpp`（不变）

```cpp
#include "ServerConfig.h"

ServerConfig g_cfg;
```

### 4.3 `src/ThreadPool.h`（新增）

```cpp
#pragma once
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <cstddef>

class ThreadPool {
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
```

### 4.4 `src/ThreadPool.cpp`（新增）

从 [Step5.cpp](../../Step1to6/Step5.cpp) 或 [step11.cpp](../../step11.cpp) 原样迁入：

```cpp
#include "ThreadPool.h"
#include <utility>

ThreadPool::ThreadPool(std::size_t threadCount) : stop_(false) {
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}
```

### 4.5 `src/HttpConnection.h`（修改）

```cpp
#pragma once

class ThreadPool;

void dispatchClient(int epfd, int client_fd, ThreadPool& pool);
```

### 4.6 `src/HttpConnection.cpp`（修改）

```cpp
#include "HttpConnection.h"
#include "ThreadPool.h"
#include "EpollHelper.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "HttpResponse.h"
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std;

static string processRawRequest(const string& rawRequest) {
    HttpRequest req;
    if (!parseRequestLine(rawRequest, req))
        return buildHttpResponse(400, "Bad Request",
            "text/plain; charset=utf-8", "请求行格式错误");
    cout << "[worker " << this_thread::get_id() << "] "
         << req.method << " " << req.path << "\n";
    return handleRequest(req);
}

void dispatchClient(int epfd, int client_fd, ThreadPool& pool) {
    char buffer[8192] = {0};
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    removeFd(epfd, client_fd);

    string rawRequest;
    if (n > 0) rawRequest.assign(buffer, static_cast<size_t>(n));

    pool.submit([client_fd, rawRequest, n]() {
        string response;
        if (n <= 0) {
            response = buildHttpResponse(400, "Bad Request",
                "text/plain; charset=utf-8", "无法读取请求");
        } else {
            response = processRawRequest(rawRequest);
        }
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "[worker] 完成 fd=" << client_fd << "\n";
    });
}
```

### 4.7 `src/main.cpp`（修改）

```cpp
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

int main() {
    ThreadPool pool(g_cfg.thread_pool_size);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        cerr << "socket 失败\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(g_cfg.port));

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "bind 失败\n";
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        cerr << "listen 失败\n";
        return 1;
    }

    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        cerr << "epoll_create1 失败\n";
        return 1;
    }

    addFd(epfd, listen_fd);

    cout << "线程池服务器已启动：http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "工作线程数: " << g_cfg.thread_pool_size << "\n";
    cout << "网站根目录: " << g_cfg.web_root << "/\n";

    vector<epoll_event> events(static_cast<size_t>(g_cfg.max_events));

    while (true) {
        int nready = epoll_wait(epfd, events.data(), g_cfg.max_events, -1);
        if (nready < 0) {
            cerr << "epoll_wait 失败\n";
            continue;
        }

        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           (sockaddr*)&client_addr, &len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        cerr << "accept 失败\n";
                        break;
                    }
                    setNonBlocking(client_fd);
                    addFd(epfd, client_fd);
                    cout << "[main] 新连接 fd=" << client_fd << "\n";
                }
            } else {
                dispatchClient(epfd, fd, pool);
            }
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
```

---

## 第五部分：Makefile

在 Step4 的 `Makefile` 基础上修改：

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Isrc -pthread
LDFLAGS  = -pthread

TARGET = server

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp \
       src/EpollHelper.cpp \
       src/HttpConnection.cpp \
       src/ThreadPool.cpp

OBJS = $(SRCS:.cpp=.o)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
```

---

## 第六部分：逐块讲解

### 6.1 `ThreadPool` 构造——启动 N 个工作线程

```cpp
for (std::size_t i = 0; i < threadCount; ++i) {
    workers_.emplace_back([this]() { workerLoop(); });
}
```

每个工作线程执行同一个 `workerLoop`：不断从队列取任务、执行。

`[this]` 捕获当前 `ThreadPool` 对象指针，让 lambda 能访问 `tasks_`、`mtx_`、`cv_`。

### 6.2 `submit`——主线程投递任务

```cpp
void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}
```

| 步骤 | 含义 |
|------|------|
| `lock_guard` | 自动加锁、出作用域解锁 |
| `push(move(task))` | 把 lambda 移入队列，避免拷贝 |
| `notify_one` | 唤醒一个正在 `wait` 的工作线程 |

**为什么先加锁再 `notify`？** 标准做法：确保任务已经入队后，再唤醒消费者，避免「醒了但队列还是空的」竞态。

### 6.3 `workerLoop`——消费者循环

```cpp
cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
```

`wait` 的第二个参数是**谓词（predicate）**：

- 队列非空 → 醒来取任务
- `stop_ == true` 且队列空 → 线程退出

这是线程池的标准优雅退出模式。

### 6.4 `dispatchClient`——主线程只做 read + 投递

```cpp
ssize_t n = read(client_fd, buffer, ...);
removeFd(epfd, client_fd);   // 关键：主线程不再监听此 fd

pool.submit([client_fd, rawRequest, n]() {
    // 工作线程：解析 + 业务 + write + close
});
```

**为什么 read 后立刻 `removeFd`？**

1. 主线程已经读完（本步假定一次 read 够）——不需要再 epoll 这个 fd
2. 工作线程将独占 `client_fd` 做 write/close
3. 避免 epoll 重复通知同一个 fd 导致重复 `dispatchClient`

**lambda 捕获了什么？**

| 捕获 | 原因 |
|------|------|
| `client_fd`（值拷贝） | 工作线程写响应、关闭连接 |
| `rawRequest`（值拷贝） | 主线程 buffer 出了作用域后仍可用 |
| `n`（值拷贝） | 判断 read 是否成功 |

### 6.5 `processRawRequest`——业务逻辑搬到工作线程

```cpp
cout << "[worker " << this_thread::get_id() << "] "
     << req.method << " " << req.path << "\n";
```

打印线程 ID，验收时能直观看到**不同工作线程**在处理不同请求。

### 6.6 时序图

```text
  浏览器                主线程                  任务队列           工作线程
    │                    │                       │                  │
    │──── TCP 连接 ─────→│ accept                │                  │
    │                    │ addFd                 │                  │
    │──── HTTP 请求 ─────→│ epoll IN              │                  │
    │                    │ read                  │                  │
    │                    │ removeFd              │                  │
    │                    │ submit ──────────────→│ push task        │
    │                    │ epoll_wait（继续）     │ notify ─────────→│ pop & run
    │                    │                       │                  │ parse + 读文件
    │←─── HTTP 响应 ───────│                       │                  │ write + close
```

### 6.7 析构函数——优雅关闭

```cpp
~ThreadPool() {
    { stop_ = true; }
    cv_.notify_all();    // 唤醒所有睡眠的工作线程
    for (auto& t : workers_) t.join();  // 等它们真正退出
}
```

`main` 里是 `while(true)` 不会正常析构 `pool`；但写上析构是良好习惯，也方便以后加信号处理优雅退出。

---

## 第七部分：编译、运行与验收

### 7.1 编译

```bash
make clean && make
```

确认 `ThreadPool.cpp` 在 `SRCS` 里，且 `-pthread` 已加。

### 7.2 运行

```bash
./server
```

终端应显示：

```text
线程池服务器已启动：http://127.0.0.1:8080
工作线程数: 4
网站根目录: www/
```

### 7.3 基本功能

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/about.html
```

应正常返回静态页面。

### 7.4 观察多线程

快速连续发多个请求：

```bash
for i in 1 2 3 4 5 6 7 8; do curl -s -o /dev/null http://127.0.0.1:8080/ & done; wait
```

服务器终端应出现**不同**的 `[worker 140234567890]` 线程 ID（数字因机器而异），说明任务被分配到不同工作线程。

### 7.5 你应该看到的日志模式

```text
[main] 新连接 fd=7
[main] 新连接 fd=8
[worker 140728394837120] GET /
[worker 140728402230784] GET /
[worker] 完成 fd=7
[worker] 完成 fd=8
```

### 7.6 成功现象 checklist

| 检查项 | 期望 |
|--------|------|
| 编译 | 无 error，可能有 `-pthread` 相关 warning 应修掉 |
| GET `/` | 200，返回 index.html |
| 并行 curl | 全部成功，不卡死 |
| 日志 | `[main]` 和 `[worker ...]` 交替出现 |

---

## 第八部分：常见问题排查

### Q1：链接错误 `undefined reference to pthread_create`

**原因：** 未加 `-pthread`。

**办法：** `CXXFLAGS` 和 `LDFLAGS` 都加上 `-pthread`。

### Q2：偶发 400 或响应错乱

**原因：** 本步仍只 `read` 一次，TCP 半包时 `parseRequestLine` 失败；或多个请求复用 fd（不应发生）。

**说明：** Step6 用 `read_buf_` + 状态机修复半包。本步若用小请求本机测试，通常不会触发。

### Q3：工作线程日志始终同一个 ID

**原因：** 请求太少、处理太快，总被同一个空闲线程抢到。

**办法：** 在 `handleRequest` 读大文件前临时 `sleep(1)`，或并发发 20 个 curl。

### Q4：`submit` 后主线程还能访问 `client_fd` 吗？

**不应该。** read 完就 `removeFd`，之后只有工作线程 touch 这个 fd。主线程若再 read/write 会竞态。

### Q5：任务队列无限增长会怎样？

高并发且工作线程处理慢时，`tasks_` 会积压，占内存。生产环境需要队列上限 + 拒绝策略；学习阶段 4 线程 + 本机测试够用。

### Q6：能否把 `read` 也放到工作线程？

可以，但需要更复杂的 fd 生命周期管理（Step6 用 `connections` map + 状态机）。本步刻意保持「主线程 read、工作线程业务」的清晰分工。

---

## 第九部分：与单文件 Step5.cpp 对照

| 单文件 `Step5.cpp` | 分文件本步 | 说明 |
|-------------------|-----------|------|
| `class ThreadPool` | `ThreadPool.h/.cpp` | 独立模块 |
| `const size_t THREAD_POOL_SIZE = 4` | `g_cfg.thread_pool_size` | 配置化 |
| `dispatchClient` | `HttpConnection.cpp` | 从 main 拆出 |
| `processRawRequest` | `HttpConnection.cpp` 内 static 函数 | 仅本模块使用 |
| `main` 里 `ThreadPool pool(...)` | `main.cpp` | 创建点不变 |
| `handleClient` | 改名为 `dispatchClient` | 语义更准确 |

逻辑与单文件 **等价**：主线程 read + removeFd + submit，工作线程 parse + 静态文件 + write + close。

---

## 第十部分：本篇小结与下一篇预告

### 你现在已经会了什么

- [x] 实现经典「任务队列 + 条件变量」线程池
- [x] Reactor 主线程与工作线程分工
- [x] 用 `std::function` + lambda 投递异步任务
- [x] 用 `this_thread::get_id()` 观察并发
- [x] `ServerConfig` 管理 `thread_pool_size`

### 本步尚未解决

| 局限 | 哪一步解决 |
|------|-----------|
| 只解析请求行，不支持 POST Body | Step6 状态机 |
| 只 read 一次，半包/粘包 | Step6 `read_buf_` + `append_read` |
| read 完就 removeFd，无法「等更多数据」 | Step6 连接保持 EPOLLIN |
| write 在工作线程可能阻塞 | Step8 主线程 EPOLLOUT 分次写 |

### 系列进度

```text
Step4 epoll 多连接
    → Step5 线程池  ← 你在这里
    → Step6 HTTP 状态机 + POST /echo
    → Step7 定时器
    → Step8 分次写
```

### 建议实验

1. 把 `g_cfg.thread_pool_size` 改成 `1`，对比 8 个并行 curl 的总耗时。
2. 在 `workerLoop` 的 `task()` 前后打印队列 `tasks_.size()`（需加锁），观察积压情况。

---

> **下一步：** [06-Step6-HttpParser状态机与echo](./06-Step6-HttpParser状态机与echo.md) —— 三态解析、半包处理、POST `/echo` 回显。
