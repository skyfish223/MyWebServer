# 分文件 Step8 —— HttpConnection 分次写 EPOLLOUT（详细版）

> **写给谁看：** 已完成 [分文件 Step7](./07-Step7-定时器.md)（空闲连接定时器 + 状态机 + 线程池）。  
> **对应单文件：** [Step1to6/step8.cpp](../../Step1to6/step8.cpp) · [08-分次写指南](../08-分次写指南.md)  
> **本篇目标：** 非阻塞 `write` **一次可能写不完**时，用 `write_buf_` + `EPOLLOUT` **分次发送**；大文件也能完整下载。  
> **本步主要修改：** `HttpTypes.h`、`HttpConnection.h/cpp`、`EpollHelper`、`main.cpp`  
> **本步不做：** 不做 `sendfile`、不做 Keep-Alive、不做 `eventfd` 唤醒 —— [Step9](./09-Step9-Log与eventfd.md) 补 eventfd 并修复浏览器转圈 bug。

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
10. [与单文件 step8.cpp 对照](#与单文件-step8cpp-对照)
11. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

---

## 一、本篇在架构中的位置

```text
Step7：工作线程 write + close（假定一次写完）
         ↓
Step8：工作线程只 do_request（备货）
       主线程 write_buf_ + process_write（发货）
       EPOLLIN + EPOLLOUT 双事件
         ↓
Step9：+ eventfd 唤醒 + Log 异步写盘
```

**铁律：同一个 client_fd 的 `write` / `close` / `epoll_ctl` 只在主线程做。**

---

## 二、本步结束后的文件树

Step8 **不新增 .cpp 文件**，扩展已有模块：

```text
src/
├── HttpTypes.h          ← WriteResult、ConnPhase、ReadyResponse
├── HttpConnection.h     ← phase_、write_buf_、write_idx_、队列声明
├── HttpConnection.cpp   ← process_write、handle_write、drain、handle_read 改造
├── EpollHelper.h/cpp    ← modFd（Step4 可能有，本步必用）
├── main.cpp             ← EPOLLOUT 分支 + drain_ready_responses
└── ...
```

---

## 第一部分：先搞懂「我们在升级什么」

### 1.1 Step7 的写有什么问题？

Step7 工作线程里：

```cpp
string response = do_request(req);
write(client_fd, response.c_str(), response.size());  // 假定一次写完
close(client_fd);
```

在非阻塞 socket 上，`write` 的返回值**不一定等于**你要发的长度：

| 返回值 | 含义 |
|--------|------|
| `n > 0` 且 `n < size` | 只写了一部分，剩下要下次再写 |
| `-1` 且 `errno == EAGAIN` | 内核发送缓冲区满，现在写不进去 |
| `n == size` | 幸运一次写完（小响应常见） |

大静态文件（几 MB 的 `big.bin`）时，一次 `write` 几乎必然写不满；若你当「已经发完」并 `close`，客户端会得到**截断的文件**。

### 1.2 生活类比：分批发货

```text
Step7：仓库一次只能装一辆车的货，你却假定「装一次 = 全部发完」

Step8：
  ① 把要发的货堆在待发区（write_buf_）
  ② 能装多少先装多少（write 返回 n）
  ③ 车满了（EAGAIN）→ 登记「可发货时叫我」（EPOLLOUT）
  ④ epoll 通知可写 → 继续装剩下的
  ⑤ 全部发完 → 关连接
```

### 1.3 本篇架构：线程池只「备货」，主线程「发货」

```text
┌─────────────────────────────────────────────────────────────┐
│  main 线程（epoll）                                          │
│    EPOLLIN  → handle_read → 请求完整                         │
│    线程池任务 → 只 do_request，结果放进 g_ready_queue         │
│    drain_ready_responses → 写入 write_buf_，尝试 process_write│
│    EPOLLOUT → handle_write → 继续 process_write              │
│    写完 → closeConnection                                    │
│    tick_expired_connections（只踢 Reading 阶段）             │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
                     ThreadPool 工作线程
                       只做 do_request（读盘 + 拼 HTTP）
                       不 write、不 close、不碰 epoll
```

### 1.4 连接阶段 `ConnPhase`

| 阶段 | 含义 | epoll 事件 |
|------|------|-----------|
| `Reading` | 正在收 HTTP 请求 | `EPOLLIN` |
| `WaitingResponse` | 请求已完整，等工作线程 `do_request` | 暂停读（`modFd(0)`） |
| `Writing` | 正在分次 `write` 响应 | `EPOLLOUT` |

```text
Reading →（请求完整）→ WaitingResponse →（响应进 queue）→ Writing →（发完）→ close
```

### 1.5 与 Step7 的关键区别

| 项目 | Step7 | Step8 |
|------|-------|-------|
| 写响应 | 工作线程一次 `write` | 主线程 **`process_write` 循环** |
| 请求完整后 | `removeFd` + 从 map 删除 + worker write | **留在 map**，等 `write_buf_` 发完再删 |
| epoll 事件 | 只有 `EPOLLIN` | `EPOLLIN` + **`EPOLLOUT`** |
| `HttpConnection` | 读 + 定时器 | 再加 **`write_buf_`、`write_idx_`、`phase_`** |
| 线程池 | 解析 + write + close | **只 do_request** |
| `tick` | 扫全部连接 | **只踢 `Reading` 阶段** |

### 1.6 对照 TinyWebServer

| 本篇 | Tiny |
|------|------|
| `write_buf_` / `write_idx_` | `m_write_buf` / `m_write_idx` |
| `process_write` | `http_conn::process_write` |
| `modFd(..., EPOLLOUT)` | `modfd` 增加写事件 |
| 主线程写 | Tiny Proactor 模式之一 |

---

## 第二部分：需要哪些新工具

### 2.1 新增 / 强化的 epoll 接口

| API | 作用 |
|-----|------|
| `EPOLLOUT` | 「发送缓冲区有空间，可以继续 write」 |
| `epoll_ctl(EPOLL_CTL_MOD, ...)` | 在 EPOLLIN 与 EPOLLOUT 之间切换 |

`EpollHelper.h` 需有：

```cpp
void modFd(int epfd, int fd, uint32_t events);
```

### 2.2 `HttpTypes.h` 新增枚举与结构体

```cpp
enum class WriteResult { Incomplete, Complete, Error };
enum class ConnPhase { Reading, WaitingResponse, Writing };

struct ReadyResponse {
    int fd;
    std::string data;
};
```

### 2.3 `HttpConnection` 新增字段

| 字段 | 含义 |
|------|------|
| `write_buf_` | 工作线程拼好的整包响应 |
| `write_idx_` | 下次 `write` 从 `write_buf_.data() + write_idx_` 开始 |
| `phase_` | 防止 Waiting/Writing 时误读新数据 |

### 2.4 工作线程 → 主线程：就绪队列

```cpp
std::queue<ReadyResponse> g_ready_queue;
std::mutex g_ready_mtx;
```

主线程每轮 epoll 后 `drain_ready_responses()`：取出队列，填入 `write_buf_`，`modFd(EPOLLOUT)`，调用 `process_write`。

> **Step8 已知问题：** 若工作线程在 `drain` **之后** 才 push，主线程可能睡在 `epoll_wait`，响应发不出去 → 浏览器转圈。Step9 用 `eventfd` 修复；Step8 靠循环末尾 `drain` **缓解**（高并发仍可能偶发）。

### 2.5 新增函数一览（均在 `HttpConnection.cpp`）

| 函数 | 职责 |
|------|------|
| **`process_write`** | 循环 write，未写完返回 Incomplete |
| **`handle_write`** | EPOLLOUT 就绪时继续写 |
| **`drain_ready_responses`** | 把线程池结果搬进 `write_buf_` 并开写 |
| **`handle_read`（改）** | 完整后 submit 任务，**不** removeFd / 不 erase |
| **主循环（改）** | 分发 EPOLLIN / EPOLLOUT；每轮 drain |

---

## 第三部分：完整代码（可直接复制）

以下与 [step11.cpp](../../step11.cpp) 第 88～92、214～227、229～238、506～570 行逻辑一致（**不含** Step9 的 `eventfd` / `Log`）。

### 3.1 完整 `src/HttpTypes.h`（Step8 版）

```cpp
#pragma once

#include <string>
#include <map>

enum class ParseState { RequestLine, Header, Content };
enum class ReadResult { Incomplete, Complete, Error };

// Step8 新增
enum class WriteResult { Incomplete, Complete, Error };
enum class ConnPhase { Reading, WaitingResponse, Writing };

struct HttpRequest {
    std::string method, path, version, body;
    std::map<std::string, std::string> headers;
    std::size_t content_length = 0;
};

// Step8 新增：工作线程 → 主线程 的响应包裹
struct ReadyResponse {
    int fd;
    std::string data;
};
```

---

### 3.2 完整 `src/HttpConnection.h`（Step8 版）

```cpp
#pragma once

#include "HttpTypes.h"
#include <ctime>
#include <string>
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

    // Step8 新增
    ConnPhase phase_ = ConnPhase::Reading;
    std::string write_buf_;
    std::size_t write_idx_ = 0;

    void refresh_active() { last_active_ = time(nullptr); }

    bool is_expired(int timeout_sec) const {
        return difftime(time(nullptr), last_active_) >= timeout_sec;
    }
};

// ── 全局就绪队列（定义在 HttpConnection.cpp）──
extern std::queue<ReadyResponse> g_ready_queue;
extern std::mutex g_ready_mtx;

// ── 连接生命周期 ──
void closeConnection(int epfd, int fd,
                     std::unordered_map<int, HttpConnection>& conns);

void tick_expired_connections(int epfd,
                              std::unordered_map<int, HttpConnection>& conns);

// ── Step8 读写核心 ──
WriteResult process_write(int epfd, HttpConnection& conn);

void handle_write(int epfd, HttpConnection& conn,
                  std::unordered_map<int, HttpConnection>& conns);

void drain_ready_responses(int epfd,
                           std::unordered_map<int, HttpConnection>& conns);

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 std::unordered_map<int, HttpConnection>& conns);
```

> 头文件还需 `#include <queue>` 和 `#include <mutex>`（或在 `.cpp` 里包含）。推荐在 `HttpConnection.h` 加：

```cpp
#include <queue>
#include <mutex>
```

---

### 3.3 完整 `src/HttpConnection.cpp`（Step8 版）

```cpp
#include "HttpConnection.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "EpollHelper.h"
#include "ServerConfig.h"
#include "ThreadPool.h"

#include <iostream>
#include <vector>
#include <unistd.h>
#include <cerrno>

using namespace std;

// ── 全局就绪队列 ───────────────────────────────────────

queue<ReadyResponse> g_ready_queue;
mutex g_ready_mtx;

// ── 构造 ──────────────────────────────────────────────

HttpConnection::HttpConnection(int fd)
    : fd_(fd), last_active_(time(nullptr)) {}

// ── 关闭连接 ──────────────────────────────────────────

void closeConnection(int epfd, int fd,
                     unordered_map<int, HttpConnection>& conns) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    removeFd(epfd, fd);
    close(fd);
    conns.erase(it);
}

// ── 定时器：Step8 只踢 Reading 阶段 ───────────────────

void tick_expired_connections(int epfd,
                              unordered_map<int, HttpConnection>& conns) {
    vector<int> expired;
    for (const auto& [fd, conn] : conns) {
        if (conn.phase_ == ConnPhase::Reading &&
            conn.is_expired(g_cfg.conn_timeout_sec))
            expired.push_back(fd);
    }
    for (int fd : expired) {
        cout << "[main] 连接超时 fd=" << fd << "\n";
        closeConnection(epfd, fd, conns);
    }
}

// ── Step8 核心：分次 write ─────────────────────────────

WriteResult process_write(int epfd, HttpConnection& conn) {
    conn.refresh_active();

    while (conn.write_idx_ < conn.write_buf_.size()) {
        size_t remaining = conn.write_buf_.size() - conn.write_idx_;
        ssize_t n = write(conn.fd_,
                          conn.write_buf_.data() + conn.write_idx_,
                          remaining);
        if (n > 0) {
            conn.write_idx_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            modFd(epfd, conn.fd_, EPOLLOUT);
            return WriteResult::Incomplete;
        }
        return WriteResult::Error;
    }
    return WriteResult::Complete;
}

void handle_write(int epfd, HttpConnection& conn,
                  unordered_map<int, HttpConnection>& conns) {
    if (conn.phase_ != ConnPhase::Writing) return;

    WriteResult wr = process_write(epfd, conn);
    if (wr == WriteResult::Complete) {
        cout << "[main] 发完 fd=" << conn.fd_
             << " 共 " << conn.write_buf_.size() << " 字节\n";
        closeConnection(epfd, conn.fd_, conns);
    } else if (wr == WriteResult::Error) {
        cout << "[main] 写失败 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
    }
    // Incomplete：保持 EPOLLOUT，等下次 epoll 唤醒
}

// ── 从就绪队列取响应并开始写 ───────────────────────────

void drain_ready_responses(int epfd,
                           unordered_map<int, HttpConnection>& conns) {
    queue<ReadyResponse> local;
    {
        lock_guard<mutex> lock(g_ready_mtx);
        swap(local, g_ready_queue);
    }

    while (!local.empty()) {
        ReadyResponse rr = move(local.front());
        local.pop();

        auto it = conns.find(rr.fd);
        if (it == conns.end()) continue;

        HttpConnection& conn = it->second;
        if (conn.phase_ != ConnPhase::WaitingResponse) continue;

        conn.write_buf_ = move(rr.data);
        conn.write_idx_ = 0;
        conn.phase_ = ConnPhase::Writing;

        cout << "[main] 开始发送 fd=" << conn.fd_
             << " 响应长度=" << conn.write_buf_.size() << "\n";

        modFd(epfd, conn.fd_, EPOLLOUT);

        WriteResult wr = process_write(epfd, conn);
        if (wr == WriteResult::Complete) {
            cout << "[main] 发完 fd=" << conn.fd_ << "\n";
            closeConnection(epfd, conn.fd_, conns);
        } else if (wr == WriteResult::Error) {
            closeConnection(epfd, conn.fd_, conns);
        }
    }
}

// ── 读事件：请求完整后 submit，不摘连接 ────────────────

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& conns) {
    if (conn.phase_ != ConnPhase::Reading) return;

    if (!append_read(conn)) {
        cout << "[main] 读失败或连接关闭 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
        return;
    }
    conn.refresh_active();

    ReadResult result = process_read(conn);
    if (result == ReadResult::Incomplete) return;

    if (result == ReadResult::Error) {
        cout << "[main] 解析错误 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
        return;
    }

    HttpRequest req = move(conn.request_);
    int client_fd = conn.fd_;
    conn.phase_ = ConnPhase::WaitingResponse;
    modFd(epfd, client_fd, 0);   // 暂停 EPOLLIN，防止误读下一请求

    cout << "[main] 请求完整 fd=" << client_fd << " "
         << req.method << " " << req.path << "\n";

    pool.submit([client_fd, req]() {
        string response = do_request(req);
        cout << "[worker " << this_thread::get_id() << "] 拼好响应 fd="
             << client_fd << " len=" << response.size() << "\n";
        lock_guard<mutex> lock(g_ready_mtx);
        g_ready_queue.push({client_fd, move(response)});
        // Step9 在此加 wake_main_thread();
    });
}
```

---

### 3.4 完整 `src/EpollHelper.h`（Step8 需 modFd）

```cpp
#pragma once
#include <cstdint>

void setNonBlocking(int fd);
void addFd(int epfd, int fd, uint32_t events = 0x001);  // EPOLLIN
void modFd(int epfd, int fd, uint32_t events);
void removeFd(int epfd, int fd);
```

**EpollHelper.cpp** 中 `modFd` 实现（与 [step11.cpp](../../step11.cpp) 494～497 行一致）：

```cpp
#include "EpollHelper.h"
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void addFd(int epfd, int fd, uint32_t events) {
    epoll_event e{};
    e.events = events;
    e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e);
}

void modFd(int epfd, int fd, uint32_t events) {
    epoll_event e{};
    e.events = events;
    e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
}

void removeFd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}
```

---

### 3.5 完整 `src/main.cpp`（Step8 主循环）

```cpp
#include <iostream>
#include <unordered_map>
#include <vector>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>

#include "ServerConfig.h"
#include "EpollHelper.h"
#include "HttpConnection.h"
#include "ThreadPool.h"

using namespace std;

int main() {
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

    cout << "Step8 分次写服务器已启动：http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "工作线程数: " << g_cfg.thread_pool_size << "\n";
    cout << "连接超时: " << g_cfg.conn_timeout_sec << " 秒（仅 Reading 阶段）\n";
    cout << "大文件测试: 在 www/ 放 big.bin 后 curl -O http://127.0.0.1:"
         << g_cfg.port << "/big.bin\n";

    vector<epoll_event> events(g_cfg.max_events);

    while (true) {
        int nready = epoll_wait(epfd, events.data(), g_cfg.max_events, -1);
        if (nready < 0) continue;

        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           (sockaddr*)&client_addr, &len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    setNonBlocking(client_fd);
                    addFd(epfd, client_fd);
                    connections.emplace(client_fd, HttpConnection(client_fd));
                    cout << "[main] 新连接 fd=" << client_fd << "\n";
                }
            } else {
                auto it = connections.find(fd);
                if (it == connections.end()) continue;

                if (ev & EPOLLIN)
                    handle_read(epfd, it->second, pool, connections);

                // handle_read 可能 closeConnection，必须重新 find
                if (ev & EPOLLOUT) {
                    it = connections.find(fd);
                    if (it != connections.end())
                        handle_write(epfd, it->second, connections);
                }
            }
        }

        // 每轮尝试 drain（Step9 用 eventfd 加强）
        drain_ready_responses(epfd, connections);
        tick_expired_connections(epfd, connections);
    }

    return 0;
}
```

---

## 第四部分：逐文件 / 逐块讲解

### 4.1 `process_write` 循环

```cpp
while (conn.write_idx_ < conn.write_buf_.size()) {
    ssize_t n = write(conn.fd_,
        conn.write_buf_.data() + conn.write_idx_,
        conn.write_buf_.size() - conn.write_idx_);
    ...
}
```

| 结果 | 处理 |
|------|------|
| `n > 0` | `write_idx_ += n`；若循环条件仍成立，继续 write |
| `n < 0` 且 `EAGAIN` | `modFd(EPOLLOUT)`，返回 `Incomplete` |
| 其它错误 | 返回 `Error` → `closeConnection` |
| 循环正常结束 | 返回 `Complete` |

**为何在 `process_write` 里 `refresh_active`？**  
Writing 阶段可能持续多轮 EPOLLOUT；刷新活跃时间避免极端情况下被误踢（Step8 的 tick 已不踢 Waiting/Writing，但 Step11 仍保留此习惯，与 step11.cpp 一致）。

### 4.2 `modFd(epfd, fd, 0)` 的含义

请求完整、进入 `WaitingResponse` 时：

```cpp
modFd(epfd, client_fd, 0);
```

表示**暂时不监听任何事件**——既不等读也不等写，直到 `drain` 把响应放进 `write_buf_` 再 `modFd(EPOLLOUT)`。

### 4.3 `drain_ready_responses` 为何 `swap` 局部队列？

```cpp
queue<ReadyResponse> local;
{ lock_guard<mutex> lock(g_ready_mtx); swap(local, g_ready_queue); }
while (!local.empty()) { ... }
```

- 锁只持有极短时间（交换指针），减少与工作线程 push 的互斥。
- 处理 `local` 时不持锁，避免 `process_write` 里慢系统调用阻塞 worker。

### 4.4 `handle_read` 为何不 erase 连接？

Step7：

```cpp
removeFd(epfd, client_fd);
conns.erase(client_fd);
pool.submit([client_fd, req]() { write(...); close(...); });
```

Step8 连接**必须留在 map**，因为：

- 主线程要知道 `write_buf_`、`write_idx_`、`phase_`
- `EPOLLOUT` 事件到来时要用 `connections[fd]` 继续写
- 发完后才 `closeConnection` 统一删 map + epoll + fd

### 4.5 主循环 EPOLLOUT 为何要重新 `find`？

```cpp
if (ev & EPOLLIN) handle_read(...);   // 可能 closeConnection
if (ev & EPOLLOUT) {
    it = connections.find(fd);         // 必须重新查
    if (it != connections.end()) handle_write(...);
}
```

同一 fd 在一轮 epoll 里可能既有 IN 又有 OUT；读失败关连接后，写分支不能再访问已 erase 的迭代器。

### 4.6 `tick` 只踢 `Reading`

```cpp
if (conn.phase_ == ConnPhase::Reading && conn.is_expired(...))
```

| 阶段 | 是否参与 tick | 原因 |
|------|--------------|------|
| `Reading` | ✅ | 僵尸半连接 |
| `WaitingResponse` | ❌ | 工作线程可能正在读大文件拼响应 |
| `Writing` | ❌ | 大文件分次写可能超过 15 秒 |

若需 Writing 也超时，Step11 可另加 `write_timeout`（本篇不做）。

### 4.7 大文件测试准备

```bash
# 在项目根目录生成 5MB 测试文件
dd if=/dev/urandom of=www/big.bin bs=1M count=5

curl -O http://127.0.0.1:8080/big.bin
ls -l big.bin    # 应与 www/big.bin 大小一致
md5sum www/big.bin big.bin   # 可选：校验一致
```

---

## 第五部分：更新 Makefile

Step8 **不增加**新 `.cpp`，Makefile 与 Step7 相同：

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -Isrc
LDFLAGS  = -pthread

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
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

---

## 第六部分：编译、运行与验收

### 6.1 编译

```bash
make clean && make
```

| 报错 | 原因 | 处理 |
|------|------|------|
| `modFd` undefined | EpollHelper 未实现 | 补 `EpollHelper.cpp` 并加入 SRCS |
| `WriteResult` 未声明 | 漏改 HttpTypes.h | 增加枚举 |
| `g_ready_queue` multiple definition | 写在 .h 里 | 只在 .cpp 定义，.h 用 `extern` |

### 6.2 功能验收

**小页面：**

```bash
curl -v http://127.0.0.1:8080/
curl -X POST http://127.0.0.1:8080/echo -d "name=test"
```

**大文件完整性：**

```bash
dd if=/dev/urandom of=www/big.bin bs=1M count=5
curl -o /tmp/out.bin http://127.0.0.1:8080/big.bin
cmp www/big.bin /tmp/out.bin && echo "OK: 文件完整"
```

**观察分次写（可选）：** 在 `process_write` 的 `EAGAIN` 分支临时加 `cout`，应看到大文件多次 EPOLLOUT。

### 6.3 验收 Checklist

- [ ] `make` 通过
- [ ] 小页面 `curl /` 正常
- [ ] POST `/echo` 正常
- [ ] `www/big.bin` 下载后大小、内容与源文件一致
- [ ] 服务器日志有 `[main] 开始发送` / `[main] 发完`
- [ ] 工作线程日志只有「拼好响应」，**没有** `write` / `close`
- [ ] `HttpTypes.h` 含 `WriteResult`、`ConnPhase`、`ReadyResponse`
- [ ] `main.cpp` 处理 `EPOLLOUT` 且 `handle_write` 前重新 `find`
- [ ] 主循环末尾调用 `drain_ready_responses`
- [ ] `tick` 只踢 `Reading` 阶段
- [ ] （已知）浏览器偶发转圈 —— Step9 用 eventfd 修复

---

## 常见问题排查

### Q1：大文件下载不完整

| 检查项 |
|--------|
| 工作线程是否仍在 `write`（应已删除） |
| `process_write` 是否在 `EAGAIN` 时 `modFd(EPOLLOUT)` |
| `handle_write` 是否在 EPOLLOUT 时继续调用 `process_write` |
| `write_idx_` 是否在每次成功 write 后累加 |

### Q2：curl 小页面正常，浏览器一直转圈

Step8 **典型 bug**：主线程睡在 `epoll_wait`，worker 已 push 但本轮 drain 已过。

| 缓解（Step8） | 根治（Step9） |
|--------------|--------------|
| 循环末尾 `drain_ready_responses` | worker push 后 `wake_main_thread()` + eventfd |
| 多访问几次可能好 | 一次即可 |

**下一篇 Step9 必做 eventfd。**

### Q3：`epoll_ctl MOD failed: Bad file descriptor`

| 原因 |
|------|
| 对已 close 的 fd 做 `modFd` |
| `handle_write` 未重新 `find`，用了已 erase 的连接 |

### Q4：WaitingResponse 永远不发

| 原因 |
|------|
| worker 未 push `g_ready_queue` |
| `drain` 里 `phase_ != WaitingResponse` 被 skip |
| fd 不一致（capture 错 client_fd） |

### Q5：连接在 Writing 阶段被 tick 踢掉

确认 `tick_expired_connections` 带有：

```cpp
conn.phase_ == ConnPhase::Reading &&
```

### Q6：POST 大 body 失败

与分次写无关，查 Step6 的 `max_body_size` 和状态机 Content 阶段。

---

## 与单文件 step8.cpp 对照

| 单文件 step8.cpp | 分文件 Step8 |
|------------------|--------------|
| `enum class WriteResult` 等 | `HttpTypes.h` |
| `ConnPhase`、`write_buf_` | `HttpConnection.h` |
| `g_ready_queue`、`g_ready_mtx` | `HttpConnection.cpp` 定义，`.h` extern |
| `process_write` / `handle_write` / `drain` | `HttpConnection.cpp` |
| `modFd` | `EpollHelper.cpp` |
| `main` EPOLLOUT 分支 | `main.cpp` |

**迁移清单：**

1. 扩展 `HttpTypes.h` 三个新类型。
2. 扩展 `HttpConnection.h` 三个新字段 + 函数声明。
3. 从 [step8.cpp](../../Step1to6/step8.cpp) 复制 `process_write` 至 `handle_read` 改造全文到 `HttpConnection.cpp`。
4. 改 `main.cpp` 处理 `EPOLLOUT` + 末尾 drain。
5. 改 `tick` 只踢 Reading。
6. 大文件验收。

---

## 本篇小结与下一篇预告

| 模块 | Step8 职责 |
|------|-----------|
| `HttpTypes.h` | `WriteResult`、`ConnPhase`、`ReadyResponse` |
| `HttpConnection` | 读 / 等响应 / 写 三阶段 + 就绪队列 |
| `EpollHelper` | `modFd(EPOLLOUT)` |
| `ThreadPool` | 只 `do_request`，不碰 fd |
| `main` | EPOLLIN + EPOLLOUT + drain + tick |

**Step8 完成后，大文件可以完整下载，写路径全部在主线程，符合 Reactor + 线程池的标准分工。**

**下一篇：** [09-Step9-Log与eventfd](./09-Step9-Log与eventfd.md) —— 新建 `Log.h/cpp` 异步写 `logs/server.log`；`eventfd` 唤醒主线程 `drain`，修复浏览器转圈；`LOG_*` 替代零散 `cout`。
