# 分文件 Step7 —— 定时器踢空闲连接（详细版）

> **写给谁看：** 已完成 [分文件 Step6](./06-Step6-HttpParser状态机与echo.md)（HTTP 三态状态机 + 线程池 + POST `/echo`），工程里已有 `HttpParser`、`HttpConnection`、`ThreadPool` 等模块。  
> **对应单文件：** [Step1to6/step7.cpp](../../Step1to6/step7.cpp) · [07-定时器指南](../07-定时器指南.md)  
> **本篇目标：** 为每个连接记录**最后活跃时间**；长时间不发数据的「僵尸连接」自动 `close`，避免占满 `fd` 和 epoll。  
> **本步主要修改：** `HttpConnection.h/cpp`、`ServerConfig.h`、`main.cpp`  
> **本步不做：** 不做 `SIGALRM`、不做升序链表定时器（Tiny 进阶方案）、不做 EPOLLOUT 分次写 —— 留到 [Step8](./08-Step8-HttpConnection分次写.md)。

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
10. [与单文件 step7.cpp 对照](#与单文件-step7cpp-对照)
11. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

---

## 一、本篇在架构中的位置

```text
分文件 Step1～6 已完成：
  main → epoll → HttpConnection → HttpParser（状态机）
                → ThreadPool → HttpHandler → HttpResponse

分文件 Step7 新增能力：
  HttpConnection.last_active_  +  tick_expired_connections()
  主循环每轮 epoll 后扫描超时 → closeConnection
```

**口诀：** 踢僵尸连接**只在主线程**做；工作线程仍只 `write + close` 已摘出 epoll 的 fd（与 Step6 相同，Step8 才改为主线程写）。

---

## 二、本步结束后的文件树

Step7 **不新增文件**，只扩展已有模块：

```text
MyWebServer/
├── src/
│   ├── main.cpp                 ← 主循环末尾调用 tick
│   ├── ServerConfig.h           ← 增加 conn_timeout_sec
│   ├── ServerConfig.cpp
│   ├── HttpTypes.h              ← 本步无新枚举
│   ├── HttpConnection.h         ← last_active_、refresh_active、is_expired
│   ├── HttpConnection.cpp       ← tick_expired、closeConnection、handle_read 小改
│   ├── EpollHelper.h / .cpp     ← **删除** Step6 的 closeConnection（改放 HttpConnection.cpp）
│   ├── HttpParser.*
│   ├── HttpHandler.*
│   ├── HttpResponse.*
│   └── ThreadPool.*
├── Makefile                     ← 本步 SRCS 不变
├── www/
└── logs/                        ← Step9 才用
```

---

## 第一部分：先搞懂「我们在升级什么」

### 1.1 Step6 的隐患在哪？

Step6 为了支持 TCP 分包和 POST，连接在「请求没收齐」时会**一直挂在 epoll 上**：

```text
client 连上 → addFd → append_read → process_read 返回 Incomplete
  → fd 继续留在 epoll，等下次 EPOLLIN
```

这对正常用户没问题，但下面几类连接会一直占资源：

| 场景 | 没有定时器时 |
|------|-------------|
| 连上后**一个字节都不发** | `fd` + epoll 条目永久占用 |
| **极慢**地发请求（半行停很久） | 同上 |
| 恶意半连接 | 容易把进程 `fd` 上限打满 |

Step6 只在「读失败 / 解析错误 / 请求完整」时关连接；**「一直挂着但不干活」** 没有处理。

### 1.2 生活类比：占座不点单

```text
客人进门占座（accept）→ 一直看菜单不发单
  → 没有定时器：桌子永远占着
  → 有定时器：超过 15 秒没动静，清桌（close + epoll_ctl DEL）

客人正常点菜（read 到数据）→ 刷新「最后活跃时间」，不会被误踢
```

### 1.3 本篇方案（最小可行）

| 方案 | 做法 | 本篇 |
|------|------|------|
| **A（推荐）** | 每连接记 `last_active_`；主循环末尾扫描超时 | ✅ |
| B（对齐 Tiny） | 升序链表 + `SIGALRM` + `socketpair` | ❌ 进阶自选 |

```text
accept 成功      → last_active_ = now
append_read 成功 → refresh_active()
每轮 epoll 后    → tick_expired_connections → 超时 closeConnection
```

### 1.4 本篇架构（分文件版）

```text
┌─────────────────────────────────────────────────────────────┐
│  main.cpp（主线程）                                          │
│  while (true) {                                              │
│      epoll_wait → accept / handle_read（HttpConnection.cpp）│
│      tick_expired_connections()   ← Step7 新增               │
│  }                                                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
                     ThreadPool（与 Step6 相同）
                     工作线程：do_request + write + close
```

### 1.5 与 Step6 的区别

| 项目 | Step6 | Step7 |
|------|-------|-------|
| `HttpConnection` | 状态机字段 | 再加 **`last_active_`** |
| `handle_read` | `append_read` 后解析 | 成功后 **`refresh_active()`** |
| 主循环 | 只处理 epoll 事件 | 末尾 **`tick_expired_connections`** |
| `ServerConfig` | `web_root` 等 | 增加 **`conn_timeout_sec`** |

### 1.6 对照 TinyWebServer

| 本篇（分文件 Step7） | Tiny |
|------|------|
| `last_active_` + 扫描 | `util_timer` 升序链表 |
| `tick_expired_connections` | `utils.tick()` |
| `refresh_active` | `adjust_timer` |
| `closeConnection` | `deal_timer` 回调 |

---

## 第二部分：需要哪些新工具

### 2.1 新增头文件

| 头文件 | 提供什么 | 用在哪 |
|--------|---------|--------|
| `<ctime>` | `time_t`、`time()`、`difftime()` | `HttpConnection.h` |

Step6 已有头文件**全部保留**。

### 2.2 扩展 `ServerConfig`

```cpp
int conn_timeout_sec = 15;   // 非活跃超时（秒），自测可改成 5
```

### 2.3 改造 `HttpConnection`

| 成员 / 方法 | 何时调用 |
|-------------|---------|
| 构造时 `last_active_ = time(nullptr)` | `accept` 后 `emplace` |
| `refresh_active()` | `append_read` 成功读到字节后 |
| `is_expired(timeout)` | `tick_expired_connections` 扫描时 |

### 2.4 从 EpollHelper **移除** closeConnection（Step6 → Step7 必做）

Step6 在 `EpollHelper.h/.cpp` 里已有 `closeConnection`。Step7 **不是新增第二份**，而是**搬家**：

| 文件 | Step7 操作 |
|------|-----------|
| `EpollHelper.h` | **删除** `closeConnection` 声明，以及仅为它服务的 `#include <unordered_map>`、`class HttpConnection` |
| `EpollHelper.cpp` | **删除** `closeConnection` 函数体；若不再用到可去掉 `#include "HttpConnection.h"` |
| `HttpConnection.h` | **保留/添加** `closeConnection` 声明 |
| `HttpConnection.cpp` | **保留/添加** `closeConnection` 实现（先 `find` 再关，见 §3.3） |

`EpollHelper` Step7 终稿应只剩四个函数：

```cpp
// EpollHelper.h —— Step7 不应再出现 closeConnection
void setNonBlocking(int fd);
void addFd(int epfd, int fd, uint32_t events = 0x001);
void modFd(int epfd, int fd, uint32_t events);
void removeFd(int epfd, int fd);
```

### 2.5 新增 / 改造函数（均在 `HttpConnection.cpp`）

| 函数 | 职责 |
|------|------|
| **`closeConnection(epfd, fd, conns)`** | 查 map → `removeFd` → `close` → `erase` |
| **`tick_expired_connections`** | 遍历 `connections`，超时则 `closeConnection` |
| **`handle_read`（小改）** | `append_read` 成功后 `conn.refresh_active()` |
| **主循环** | 每轮事件处理完后调用 `tick` |

### 2.6 线程安全

| 线程 | 关「仍在 epoll 上」的 fd |
|------|-------------------------|
| **主线程** | ✅ 定时器、`handle_read` 读失败 |
| **工作线程** | ❌ 只关已 `removeFd` 后 submit 的 fd |

---

## 第三部分：完整代码（可直接复制）

以下代码与 [step11.cpp](../../step11.cpp) 中定时器相关逻辑一致；Step7 阶段尚未引入 `ConnPhase`、`write_buf_`（Step8 才加）。

### 3.1 扩展 `src/ServerConfig.h`

在已有 `struct ServerConfig` 中增加一行：

```cpp
#pragma once
#include <string>

struct ServerConfig {
    std::string web_root = "www";
    int port = 8080;
    std::size_t thread_pool_size = 4;
    int max_events = 64;
    std::size_t max_body_size = 1024 * 1024;

    // Step7 新增：空闲连接超时（秒）
    int conn_timeout_sec = 15;
};

extern ServerConfig g_cfg;
```

`ServerConfig.cpp` 中 `g_cfg` 定义不变，默认值已在头文件里给出。

---

### 3.2 完整 `src/HttpConnection.h`（Step7 版）

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

    // Step7 新增：最后活跃时间
    time_t last_active_;

    void refresh_active() { last_active_ = time(nullptr); }

    bool is_expired(int timeout_sec) const {
        return difftime(time(nullptr), last_active_) >= timeout_sec;
    }
};

// Step7：连接关闭与定时器（实现在 HttpConnection.cpp）
void closeConnection(int epfd, int fd,
                     std::unordered_map<int, HttpConnection>& conns);

void tick_expired_connections(int epfd,
                              std::unordered_map<int, HttpConnection>& conns);

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 std::unordered_map<int, HttpConnection>& conns);
```

---

### 3.3 完整 `src/HttpConnection.cpp`（Step7 版）

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

using namespace std;

// ── 构造 ──────────────────────────────────────────────

HttpConnection::HttpConnection(int fd)
    : fd_(fd), last_active_(time(nullptr)) {}

// ── 关闭连接（带 map，防重复 close）────────────────────

void closeConnection(int epfd, int fd,
                     unordered_map<int, HttpConnection>& conns) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    removeFd(epfd, fd);
    close(fd);
    conns.erase(it);
}

// ── Step7 核心：扫描超时连接 ───────────────────────────

void tick_expired_connections(int epfd,
                              unordered_map<int, HttpConnection>& conns) {
    vector<int> expired;
    expired.reserve(conns.size());

    for (const auto& [fd, conn] : conns) {
        if (conn.is_expired(g_cfg.conn_timeout_sec))
            expired.push_back(fd);
    }

    for (int fd : expired) {
        cout << "[main] 连接超时 fd=" << fd << "\n";
        closeConnection(epfd, fd, conns);
    }
}

// ── 读事件处理（Step6 逻辑 + refresh_active）────────────

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& conns) {
    if (!append_read(conn)) {
        cout << "[main] 读失败或连接关闭 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
        return;
    }

    // Step7：每读到新数据就刷新活跃时间
    conn.refresh_active();

    ReadResult result = process_read(conn);

    if (result == ReadResult::Incomplete)
        return;

    if (result == ReadResult::Error) {
        cout << "[main] 解析错误 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, conns);
        return;
    }

    // 请求完整：与 Step6 相同，摘出 epoll 交给工作线程
    HttpRequest req = move(conn.request_);
    int client_fd = conn.fd_;
    removeFd(epfd, client_fd);
    conns.erase(client_fd);

    pool.submit([client_fd, req]() {
        cout << "[worker " << this_thread::get_id() << "] "
             << req.method << " " << req.path;
        if (!req.body.empty())
            cout << " body_len=" << req.body.size();
        cout << "\n";

        string response = do_request(req);
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "[worker] 完成 fd=" << client_fd << "\n";
    });
}
```

> **说明：** Step7 仍沿用 Step6 的「工作线程 `write + close`」模型。Step8 会改为连接留在 map、主线程分次写；Step7 的 `tick` 在 Step8 起会加上 `phase_ == Reading` 判断（本篇先扫全部连接，与单文件 step7 一致）。

---

### 3.4 完整 `src/main.cpp`（Step7 关键片段）

在 Step6 的 `main.cpp` 基础上，**主循环末尾**增加一行：

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
    if (listen_fd < 0) {
        cerr << "socket 失败\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_cfg.port);

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

    cout << "Step7 定时器服务器已启动：http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "工作线程数: " << g_cfg.thread_pool_size << "\n";
    cout << "连接超时: " << g_cfg.conn_timeout_sec << " 秒\n";
    cout << "网站根目录: " << g_cfg.web_root << "/\n";
    cout << "超时测试: nc 127.0.0.1 " << g_cfg.port
         << "（连上后不发数据，等 " << g_cfg.conn_timeout_sec << " 秒）\n";

    vector<epoll_event> events(g_cfg.max_events);

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
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        cerr << "accept 失败\n";
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
                handle_read(epfd, it->second, pool, connections);
            }
        }

        // ── Step7 新增：每轮 epoll 后踢僵尸连接 ──
        tick_expired_connections(epfd, connections);
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
```

---

### 3.5 `HttpTypes.h` 本步不变

Step7 不新增枚举，保持 Step6 内容即可：

```cpp
#pragma once
#include <string>
#include <map>

enum class ParseState { RequestLine, Header, Content };
enum class ReadResult { Incomplete, Complete, Error };

struct HttpRequest {
    std::string method, path, version, body;
    std::map<std::string, std::string> headers;
    std::size_t content_length = 0;
};
```

---

## 第四部分：逐文件 / 逐块讲解

### 4.1 `HttpConnection` 构造与 `last_active_`

```cpp
explicit HttpConnection(int fd) : fd_(fd), last_active_(time(nullptr)) {}
```

- `accept` 成功、`emplace` 进 map 的瞬间记一次时间。
- 若客户端连上后 15 秒内一个字节都不发，`tick` 会踢掉它。

### 4.2 `refresh_active()` 放在哪？

**必须**在 `append_read` **成功读到数据之后**调用：

```cpp
if (!append_read(conn)) { /* 关连接 */ return; }
conn.refresh_active();   // ← 这里
ReadResult result = process_read(conn);
```

| 调用点 | 要不要 refresh |
|--------|----------------|
| `append_read` 成功 | ✅ |
| `process_read` 返回 Incomplete | 已在上面 refresh 过，不必重复 |
| 请求完整、交给线程池 | 本步连接已摘出 epoll，不再参与 tick |
| 工作线程 write | 本步 worker 写，主线程 tick 扫不到该 fd |

### 4.3 `is_expired` 与 `difftime`

```cpp
bool is_expired(int timeout_sec) const {
    return difftime(time(nullptr), last_active_) >= timeout_sec;
}
```

- `difftime(t1, t2)` 返回 `t1 - t2` 的秒数（`double`）。
- `>= timeout_sec` 表示「距离上次活跃已超过阈值」。
- 自测时可在 `ServerConfig.h` 把 `conn_timeout_sec` 改成 `5`，不用等 15 秒。

### 4.4 `tick_expired_connections` 为何先收集再关？

```cpp
vector<int> expired;
for (const auto& [fd, conn] : conns)
    if (conn.is_expired(...)) expired.push_back(fd);
for (int fd : expired)
    closeConnection(epfd, fd, conns);
```

若在 `for (auto& [fd, c] : conns)` 里直接 `erase`，会**迭代器失效**。先收集 fd 列表，再逐个关闭，是安全写法。

### 4.5 `closeConnection` 为何要查 map？

同一 fd 可能因「读失败」和「定时器」两条路径都想关；先 `find` 再操作，避免：

- 对同一 fd 重复 `close`（未定义行为）
- 对不存在的 fd 重复 `epoll_ctl(DEL)`

与 [step11.cpp](../../step11.cpp) 第 500～504 行一致：

```cpp
void closeConnection(int epfd, int fd, unordered_map<int, HttpConnection>& conns) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    removeFd(epfd, fd); close(fd); conns.erase(it);
}
```

### 4.6 主循环里 `tick` 的位置

```text
for (每个 epoll 就绪事件) { ... 处理 accept / read ... }
tick_expired_connections(epfd, connections);   // 必须在 for 循环之外
```

放在**每轮 `epoll_wait` 返回之后、下一轮等待之前**。这样即使本轮没有任何 EPOLLIN（例如只有一个慢连接挂着），也会执行超时检查。

### 4.7 为何 tick 放主线程而不是工作线程？

| 若放工作线程 | 问题 |
|-------------|------|
| 多个 worker 同时扫 map | 需要额外锁，易与 epoll 竞态 |
| worker 里 `epoll_ctl(DEL)` | 违反「epoll 只在主线程」约定（Step8 起更严格） |

**结论：** 与 Tiny 的 `tick` 在 reactor 线程执行一样，扫描式定时器跟 epoll 主循环绑定。

---

## 第五部分：更新 Makefile

Step7 **不增加**新的 `.cpp`，Makefile 与 Step6 相同。若你从零跟写，此时应类似：

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

编译仍在**项目根目录**（与 `www/` 同级）：

```bash
make
./server
```

---

## 第六部分：编译、运行与验收

### 6.1 编译

```bash
cd /path/to/MyWebServer
make clean && make
```

常见报错：

| 报错 | 原因 | 处理 |
|------|------|------|
| `multiple definition of closeConnection` | Step6 的 `EpollHelper.cpp` 与 Step7 的 `HttpConnection.cpp` **各有一份**实现 | 按 §2.4 **删掉 EpollHelper 里那份**，只留 `HttpConnection.cpp` |
| `undefined reference to tick_expired_connections` | 函数只有声明无实现 | 检查 `HttpConnection.cpp` 是否加入 `SRCS` |
| `'time_t' does not name a type` | 漏 `#include <ctime>` | 加到 `HttpConnection.h` |
| `conn_timeout_sec` 未定义 | 漏改 `ServerConfig.h` | 增加字段并 `#include "ServerConfig.h"` |

### 6.2 功能验收

**正常请求（不应被误踢）：**

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/echo
curl -X POST http://127.0.0.1:8080/echo -d "hello=world"
```

**超时踢连接：**

```bash
# 终端 1：启动服务器
./server

# 终端 2：连上但不发数据
nc 127.0.0.1 8080
# 等待 conn_timeout_sec 秒（默认 15），nc 应断开
# 服务器终端应打印：[main] 连接超时 fd=...
```

**慢速发包（有数据就不踢）：**

```bash
# 每 5 秒发一行，总时间超过 15 秒也不应被踢（因为 refresh_active）
( sleep 5; echo -ne "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n" ) | nc 127.0.0.1 8080
```

### 6.3 验收 Checklist

- [ ] `make` 无警告通过
- [ ] `curl /` 返回 200 和 HTML
- [ ] `curl -X POST /echo -d "a=b"` 返回 body 内容
- [ ] `nc` 连上不发数据，超过 `conn_timeout_sec` 后连接被关闭
- [ ] 服务器打印 `连接超时 fd=...`
- [ ] 正常浏览器访问 `/echo` 表单页无异常
- [ ] 多开几个 `curl` 并行，均能正常响应
- [ ] `HttpConnection.cpp` 中有 `closeConnection` 和 `tick_expired_connections`
- [ ] `main.cpp` 主循环末尾调用 `tick_expired_connections`

---

## 常见问题排查

### Q1：`nc` 连上后一直不断开

| 可能原因 | 排查 |
|---------|------|
| 主循环未调用 `tick` | 搜索 `tick_expired_connections`，确认在 `while` 内、`for` 外 |
| 超时时间太长 | 临时改 `conn_timeout_sec = 5` |
| `refresh_active` 被误调用 | 确认只在 `append_read` 成功后有 refresh |
| 连接不在 `connections` map | 请求完整后已 erase，worker 里的 fd 不会被 tick（正常） |

### Q2：正常请求中途被踢

| 可能原因 | 排查 |
|---------|------|
| 超时太短 | 调大 `conn_timeout_sec` |
| 大 POST body 传输超过超时 | 慢网 POST 时每次 `append_read` 应 refresh；检查是否漏掉 |
| `max_body_size` 太小导致长时间 Incomplete | 与定时器无关，先查 Step6 配置 |

### Q3：编译报 `difftime` 未声明

在 `HttpConnection.h` 或 `.cpp` 增加 `#include <ctime>`。

### Q4：`closeConnection` 和 `EpollHelper` 里重复定义（链接阶段）

**典型报错：**

```text
multiple definition of `closeConnection(...)`;
EpollHelper.o: first defined here
HttpConnection.o: ... also defined here
```

**原因：** Step6 把 `closeConnection` 写在 `EpollHelper.cpp`；Step7 又在 `HttpConnection.cpp` 写了一份，**忘记删除旧的那份**。

**处理（按顺序做）：**

1. 打开 `EpollHelper.h`，删掉 `closeConnection` 那一行声明。
2. 打开 `EpollHelper.cpp`，删掉整个 `closeConnection` 函数。
3. 确认 `HttpConnection.h` 有声明、`HttpConnection.cpp` 有实现（带 `conns.find(fd)` 检查）。
4. `make clean && make`。

`closeConnection(epfd, fd, conns)` **全工程只能有一处定义**，放在 `HttpConnection.cpp`；`EpollHelper` 只提供 `removeFd`。

### Q5：与 step7.cpp 行为不一致

单文件 [step7.cpp](../../Step1to6/step7.cpp) 的 `tick` **不区分** `ConnPhase`（Step7 还没有 phase）。  
[step8.cpp](../../Step1to6/step8.cpp) 起改为只踢 `Reading` 阶段。分文件 Step7 对齐 step7；Step8 文档会更新 tick 条件。

### Q6：能否用 `epoll_wait` 超时代替扫描？

可以（例如 `epoll_wait(epfd, ..., 1000)` 每 1 秒醒一次再 tick），但：

- 增加无效唤醒
- 与「有事件才处理」的模型略不同

本篇采用「每轮事件处理完再 tick」，与单文件教程一致，逻辑最简单。

---

## 与单文件 step7.cpp 对照

| 单文件 step7.cpp | 分文件 Step7 |
|------------------|--------------|
| `const int CONN_TIMEOUT_SEC = 15` | `g_cfg.conn_timeout_sec` in `ServerConfig.h` |
| `HttpConnection` 内联方法 | `HttpConnection.h` 声明 |
| `tick_expired_connections` | `HttpConnection.cpp` |
| `handle_read` | `HttpConnection.cpp` |
| `closeConnection` | `HttpConnection.cpp`（依赖 `EpollHelper::removeFd`） |
| `main` 主循环 | `main.cpp` |

**迁移步骤：**

0. **从 `EpollHelper.h/.cpp` 删除 Step6 的 `closeConnection`**（见 §2.4，避免链接重复定义）。
1. 从 [step7.cpp](../../Step1to6/step7.cpp) 复制 `last_active_`、`refresh_active`、`is_expired` 到 `HttpConnection.h`。
2. 复制 `tick_expired_connections`、`closeConnection` 到 `HttpConnection.cpp`。
3. 在 `handle_read` 的 `append_read` 后加 `refresh_active()`。
4. 在 `main.cpp` 循环末尾加 `tick_expired_connections`。
5. `make clean && make && ./server` 跑验收命令。

逻辑与单文件**完全一致**，只是物理文件不同。

---

## 本篇小结与下一篇预告

| 能力 | 所在文件 |
|------|----------|
| 最后活跃时间 | `HttpConnection.h` |
| 空闲断连 | `HttpConnection.cpp` → `tick_expired_connections` |
| 超时秒数配置 | `ServerConfig.h` → `conn_timeout_sec` |
| 主循环调度 | `main.cpp` |

**Step7 完成后，你的服务器能自动清理占座不点单的僵尸连接，fd 不会被半连接慢慢耗尽。**

**下一篇：** [08-Step8-HttpConnection分次写](./08-Step8-HttpConnection分次写.md) —— 非阻塞 `write` 一次写不完时用 `write_buf_` + `EPOLLOUT` 分次发送；工作线程只拼响应，主线程负责写；引入 `ConnPhase` 与 `g_ready_queue`。
