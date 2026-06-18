# 分文件 Step4 —— epoll 与 EpollHelper

> **写给谁看：** 已完成 [Step3 分文件版](./03-Step3-HttpResponse静态文件.md) 的学习者——`www/` 静态文件能正常访问。  
> **对应单文件：** [Step1to6/Step4.cpp](../../Step1to6/Step4.cpp) · [04-epoll高并发指南](../04-epoll高并发指南.md)  
> **本篇目标：** 主线程 `epoll_wait` 同时监视多个连接；`accept` / `read` / `write` 全部非阻塞。  
> **本步新增：** `EpollHelper`、`HttpConnection`（雏形）；**大改 `main.cpp`**；**扩展 `ServerConfig`**

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
9. [第九部分：与单文件 Step4.cpp 对照](#第九部分与单文件-step4cpp-对照)
10. [第十部分：本篇小结与下一篇预告](#第十部分本篇小结与下一篇预告)

---

## 第一部分：架构说明——我们在升级什么

### 1.1 Step3 的瓶颈

Step3 的主循环是**阻塞串行**模型：

```text
while (true) {
    accept()          ← 没客人时，程序卡死在这里
    read → 处理 → write → close
    再 accept 下一个  ← 前一个没处理完，后面的在排队
}
```

| 问题 | 说明 |
|------|------|
| **串行** | 同一时刻只为**一个**连接做 read/write |
| **accept 阻塞** | 处理 A 客人时，B 客人的 TCP 握手完成了也进不来 |
| **无法并发** | 多个 curl 同时访问时，后面的请求必须等前面的读完文件、写完响应 |

对小作业、自己浏览器测一下**够用**；但要同时处理很多连接，就需要 **I/O 多路复用**（本篇的 epoll）。

### 1.2 生活类比：一个服务员怎么盯多桌？

| 模式 | 类比 | Step3 |
|------|------|-------|
| **串行** | 服务员盯完 1 号桌才去看 2 号桌 | `accept` → 处理完 → 再 `accept` |
| **epoll** | 服务员站在大厅中央，**哪桌举手（有数据）就去哪桌** | `epoll_wait` 一次返回多个「就绪」的 fd |

### 1.3 本篇架构（Reactor 雏形）

```text
┌─────────────────────────────────────────────────────────────┐
│  main 线程（唯一的 I/O 线程）                                 │
│                                                             │
│  epoll_create1 → epoll_ctl 注册 listen_fd                   │
│       │                                                     │
│       ▼                                                     │
│  while (true) {                                             │
│      epoll_wait  ← 阻塞，直到有 fd 就绪                      │
│      for 每个就绪 fd {                                       │
│          listen_fd  → accept 循环，新 fd 加入 epoll          │
│          client_fd  → read → 解析 → 读 www → write → close │
│      }                                                      │
│  }                                                          │
└─────────────────────────────────────────────────────────────┘
```

**业务逻辑（解析 HTTP、读 www）和 Step3 相同**，变的只是 **「等事件、分发连接」** 的方式。

### 1.4 分文件后的模块职责

| 模块 | 本步职责 |
|------|---------|
| `EpollHelper` | 非阻塞、`epoll_ctl` 增删改、关闭连接辅助 |
| `HttpConnection` | 封装「处理一个客户端可读事件」的逻辑（本步仍同步处理） |
| `main.cpp` | `epoll_wait` 主循环 + `accept` 风暴 |
| `ServerConfig` | 新增 `port`、`max_events` |
| `HttpParser` / `HttpResponse` / `HttpHandler` | 与 Step3 相同，未改 |

### 1.5 本篇程序整体流程

```text
  socket → bind → listen（同 Step3）
     │
     ▼
  setNonBlocking(listen_fd)
     │
     ▼
  epoll_create1 → addFd(epfd, listen_fd)
     │
     ▼
  while (true) {
        epoll_wait
        for 每个就绪 fd {
            listen_fd  → while accept 直到 EAGAIN
                         → setNonBlocking → addFd
            client_fd  → handleClient：read → parse → 静态文件 → write → close
        }
     }
```

> **注意：** 本步仍在**主线程同步**完成 read + 业务 + write。Step5 才把业务扔到线程池。

---

## 第二部分：需要哪些新工具

### 2.1 头文件一览

| 头文件 | 提供什么 | 为什么需要 |
|--------|---------|-----------|
| `<fcntl.h>` | `fcntl`、`O_NONBLOCK` | 把 socket 设为非阻塞 |
| `<sys/epoll.h>` | `epoll_create1`、`epoll_ctl`、`epoll_wait` | I/O 多路复用核心 API |
| `<cerrno>` | `errno`、`EAGAIN`、`EWOULDBLOCK` | 非阻塞 `accept`/`read` 返回「暂时没数据」时判断 |
| `<vector>` | `vector<epoll_event>` | 存放 `epoll_wait` 返回的就绪事件 |

### 2.2 epoll 三个核心 API

| API | 一句话 |
|-----|--------|
| `epoll_create1(0)` | 创建 epoll 实例（空监视器），返回 `epfd` |
| `epoll_ctl(epfd, op, fd, &ev)` | 往监视名单 **添加(ADD) / 修改(MOD) / 删除(DEL)** 某个 fd |
| `epoll_wait(epfd, events, max, timeout)` | **阻塞等待**，直到名单里至少有一个 fd「就绪」 |

### 2.3 非阻塞 I/O 要点

把 fd 设为 `O_NONBLOCK` 后：

| 系统调用 | 阻塞模式 | 非阻塞模式 |
|---------|---------|-----------|
| `accept` | 没连接时睡眠 | 立刻返回 `-1`，`errno == EAGAIN` |
| `read` | 没数据时睡眠 | 立刻返回 `-1`，`errno == EAGAIN` |

因此 `accept` 要写成 **while 循环**——一次 `epoll_wait` 通知监听 fd 就绪，可能同时有多个连接在排队，要全部 `accept` 出来。

### 2.4 水平触发（LT）

本篇使用 epoll 默认的 **LT（水平触发）** 模式：

- 缓冲区里**还有数据没读完**，下次 `epoll_wait` 仍会通知这个 fd 可读
- 比 ET（边缘触发）更宽容，适合学习阶段

---

## 第三部分：完整文件树

本步结束时，工程结构如下（**粗体**为新增或本步大改）：

```text
MyWebServer/
├── Makefile
├── src/
│   ├── main.cpp                 ← 大改：epoll 主循环
│   ├── ServerConfig.h           ← 扩展 port、max_events
│   ├── ServerConfig.cpp
│   ├── HttpTypes.h
│   ├── HttpParser.h
│   ├── HttpParser.cpp
│   ├── HttpResponse.h
│   ├── HttpResponse.cpp
│   ├── HttpHandler.h
│   ├── HttpHandler.cpp
│   ├── EpollHelper.h            ← 新增
│   ├── EpollHelper.cpp          ← 新增
│   ├── HttpConnection.h         ← 新增
│   └── HttpConnection.cpp       ← 新增
└── www/                         （Step3 已有，本步继续用）
    ├── index.html
    ├── about.html
    └── css/style.css
```

---

## 第四部分：完整源码（逐步粘贴）

下面列出本步**所有**源文件。从 Step3 延续的文件也完整给出，方便你对照粘贴，不必翻回 Step3 文档。

### 4.1 `src/ServerConfig.h`

```cpp
#pragma once
#include <string>

struct ServerConfig {
    std::string web_root = "www";
    int port = 8080;
    int max_events = 64;
};

extern ServerConfig g_cfg;
```

### 4.2 `src/ServerConfig.cpp`

```cpp
#include "ServerConfig.h"

ServerConfig g_cfg;
```

### 4.3 `src/HttpTypes.h`

```cpp
#pragma once
#include <string>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};
```

### 4.4 `src/HttpParser.h`

```cpp
#pragma once
#include "HttpTypes.h"
#include <string>

bool parseRequestLine(const std::string& raw, HttpRequest& req);
```

### 4.5 `src/HttpParser.cpp`

```cpp
#include "HttpParser.h"
#include <sstream>

using namespace std;

bool parseRequestLine(const string& raw, HttpRequest& req) {
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == string::npos) lineEnd = raw.find('\n');
    if (lineEnd == string::npos) return false;

    string line = raw.substr(0, lineEnd);
    istringstream iss(line);
    return static_cast<bool>(iss >> req.method >> req.path >> req.version);
}
```

### 4.6 `src/HttpResponse.h`

```cpp
#pragma once
#include <string>

std::string buildHttpResponse(int statusCode,
                              const std::string& statusText,
                              const std::string& contentType,
                              const std::string& body);

std::string pathToFile(const std::string& urlPath);
bool isPathSafe(const std::string& path);
std::string getContentType(const std::string& filePath);
bool readFile(const std::string& filePath, std::string& out);
```

### 4.7 `src/HttpResponse.cpp`

```cpp
#include "HttpResponse.h"
#include "ServerConfig.h"
#include <fstream>
#include <iterator>

using namespace std;

string buildHttpResponse(int statusCode,
                         const string& statusText,
                         const string& contentType,
                         const string& body) {
    return "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html")
        return g_cfg.web_root + "/index.html";
    return g_cfg.web_root + urlPath;
}

bool isPathSafe(const string& urlPath) {
    return urlPath.find("..") == string::npos;
}

string getContentType(const string& filePath) {
    if (filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".html")
        return "text/html; charset=utf-8";
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".css")
        return "text/css; charset=utf-8";
    if (filePath.size() >= 3 && filePath.substr(filePath.size() - 3) == ".js")
        return "application/javascript; charset=utf-8";
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".png")
        return "image/png";
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".jpg")
        return "image/jpeg";
    return "application/octet-stream";
}

bool readFile(const string& filePath, string& out) {
    ifstream ifs(filePath, ios::binary);
    if (!ifs) return false;
    out.assign(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
    return true;
}
```

### 4.8 `src/HttpHandler.h`

```cpp
#pragma once
#include "HttpTypes.h"
#include <string>

std::string handleRequest(const HttpRequest& req);
```

### 4.9 `src/HttpHandler.cpp`

```cpp
#include "HttpHandler.h"
#include "HttpResponse.h"

using namespace std;

string handleRequest(const HttpRequest& req) {
    if (req.method != "GET")
        return buildHttpResponse(405, "Method Not Allowed",
            "text/plain; charset=utf-8", "暂只支持 GET");

    if (!isPathSafe(req.path))
        return buildHttpResponse(403, "Forbidden",
            "text/plain; charset=utf-8", "403 - 非法路径");

    string filePath = pathToFile(req.path);
    string body;
    if (!readFile(filePath, body))
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "404 - 文件不存在: " + req.path);

    return buildHttpResponse(200, "OK", getContentType(filePath), body);
}
```

### 4.10 `src/EpollHelper.h`（本步新增）

```cpp
#pragma once
#include <cstdint>

void setNonBlocking(int fd);
void addFd(int epfd, int fd, uint32_t events = 0x001);
void modFd(int epfd, int fd, uint32_t events);
void removeFd(int epfd, int fd);
```

> `0x001` 即 `EPOLLIN`（关心可读事件）。写成数字是为了头文件不必 `#include <sys/epoll.h>`。

### 4.11 `src/EpollHelper.cpp`（本步新增）

```cpp
#include "EpollHelper.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void addFd(int epfd, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void modFd(int epfd, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

void removeFd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}
```

> `modFd` 本步暂未用到，但提前写好——Step8 分次写响应时会改监听 `EPOLLOUT`。写法参考 [step11.cpp](../../step11.cpp) 中的 `addFd` / `modFd`。

### 4.12 `src/HttpConnection.h`（本步新增）

```cpp
#pragma once

void handleClient(int epfd, int client_fd);
```

### 4.13 `src/HttpConnection.cpp`（本步新增）

```cpp
#include "HttpConnection.h"
#include "EpollHelper.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "HttpResponse.h"
#include <iostream>
#include <string>
#include <unistd.h>

using namespace std;

void handleClient(int epfd, int client_fd) {
    char buffer[8192] = {0};
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    string response;
    if (n <= 0) {
        response = buildHttpResponse(400, "Bad Request",
            "text/plain; charset=utf-8", "无法读取请求");
    } else {
        string rawRequest(buffer, n);
        HttpRequest req;
        if (!parseRequestLine(rawRequest, req)) {
            response = buildHttpResponse(400, "Bad Request",
                "text/plain; charset=utf-8", "请求行格式错误");
        } else {
            cout << "[fd" << client_fd << "] " << req.method << " " << req.path << endl;
            response = handleRequest(req);
        }
    }

    write(client_fd, response.c_str(), response.size());
    removeFd(epfd, client_fd);
    close(client_fd);
}
```

### 4.14 `src/main.cpp`（本步大改）

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

using namespace std;

int main() {
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

    cout << "epoll 服务器已启动：http://127.0.0.1:" << g_cfg.port << "\n";
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
                    cout << "新连接 fd=" << client_fd << "\n";
                }
            } else {
                handleClient(epfd, fd);
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

在项目根目录创建或更新 `Makefile`：

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Isrc
LDFLAGS  =

TARGET = server

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp \
       src/EpollHelper.cpp \
       src/HttpConnection.cpp

OBJS = $(SRCS:.cpp=.o)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
```

| 选项 | 含义 |
|------|------|
| `-Isrc` | 让 `#include "HttpParser.h"` 等能找到 `src/` 下的头文件 |
| `-std=c++17` | 使用 C++17（`{}` 初始化、`static_cast` 等） |

---

## 第六部分：逐块讲解

### 6.1 `setNonBlocking`——把 fd 变成「不等待」

```cpp
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

| 步骤 | 含义 |
|------|------|
| `F_GETFL` | 读出 fd 当前标志位 |
| `O_NONBLOCK` | 非阻塞标志：I/O 操作不睡眠 |
| `F_SETFL` | 写回新标志 |

**为什么 listen_fd 也要非阻塞？**  
`epoll` 通知监听 fd 可读，表示「至少有一个连接在排队」。要用 `while (accept)` 循环全部接完；非阻塞模式下，接完最后一个后 `accept` 返回 `EAGAIN`，循环退出。

### 6.2 `addFd` / `removeFd`——管理监视名单

```cpp
epoll_event ev{};
ev.events = EPOLLIN;      // 只关心「可读」
ev.data.fd = fd;          // 就绪时，用这个字段知道是哪个 fd
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
```

`ev.data.fd = fd` 是关键：后面 `epoll_wait` 返回时，通过 `events[i].data.fd` 判断是监听 socket 还是客户端 socket。

`removeFd` 在关闭连接前调用，把 fd 从 epoll 摘掉，避免对已关闭的 fd 继续 `epoll_ctl`。

### 6.3 `accept` 风暴——一次事件接多个连接

```cpp
while (true) {
    int client_fd = accept(...);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        ...
    }
    setNonBlocking(client_fd);
    addFd(epfd, client_fd);
}
```

**为什么不能只 `accept` 一次？**  
高并发时，内核等待队列里可能堆了多个已完成握手的连接。`epoll` 只通知一次「监听 fd 可读」，你必须循环 `accept` 直到 `EAGAIN` 才接干净。

### 6.4 `handleClient`——本步仍在主线程同步处理

```text
read 一次（最多 8191 字节）
  → parseRequestLine（只解析第一行，同 Step3）
  → handleRequest（读 www 静态文件）
  → write 整段响应
  → removeFd + close
```

本步**没有** `connections` 映射表，也**没有**线程池。客户端 fd 注册到 epoll 后，等它可读时一次性处理完并关闭。

### 6.5 `ServerConfig` 扩展的意义

| 字段 | 默认值 | 作用 |
|------|--------|------|
| `web_root` | `"www"` | Step3 已有 |
| `port` | `8080` | 端口不再写死在 `main` 里 |
| `max_events` | `64` | `epoll_wait` 一次最多返回多少个就绪事件 |

Step11 会继续往 `ServerConfig` 里加线程数、数据库配置等。

### 6.6 主循环结构图

```text
                    ┌──────────────┐
                    │  epoll_wait  │
                    └──────┬───────┘
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
        fd == listen_fd           fd == client_fd
              │                         │
              ▼                         ▼
     while accept 直到 EAGAIN      handleClient
     setNonBlocking + addFd        read→parse→write→close
```

---

## 第七部分：编译、运行与验收

### 7.1 环境要求

- **Linux** 或 **WSL2**
- g++：`sudo apt install g++ make`

### 7.2 编译

在项目根目录（能看到 `src/` 和 `www/`）：

```bash
make clean && make
```

应无警告、无错误，生成可执行文件 `./server`。

### 7.3 运行

```bash
./server
```

终端应显示：

```text
epoll 服务器已启动：http://127.0.0.1:8080
网站根目录: www/
```

### 7.4 基本功能测试

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/about.html
curl http://127.0.0.1:8080/css/style.css
```

应分别返回 `www/index.html`、`www/about.html`、`www/css/style.css` 的内容。

### 7.5 并发测试

开两个终端，**同时**执行：

```bash
curl http://127.0.0.1:8080/
```

或用 bash 并行：

```bash
for i in 1 2 3 4 5; do curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/ & done; wait
```

五个请求都应返回 `200`。服务器终端应看到多个 `新连接 fd=...` 和 `[fd...] GET /` 日志。

### 7.6 你应该看到的成功现象

| 位置 | 现象 |
|------|------|
| `make` | 编译通过，生成 `./server` |
| 浏览器访问 `http://127.0.0.1:8080/` | 显示 `www/index.html` 页面 |
| 并行 curl | 全部 200，不会卡住 |
| 服务器终端 | `新连接 fd=N` → `[fdN] GET /path` |

---

## 第八部分：常见问题排查

### Q1：`epoll_create1 失败`

**原因：** 系统资源不足，或不在 Linux/WSL 环境（Windows 原生不支持 epoll）。

**办法：** 确认在 WSL/Linux 下编译运行；检查 `ulimit -n` 是否过低。

### Q2：`accept 失败`（非 EAGAIN）

**原因：** 监听 fd 已损坏，或进程 fd 数量达到上限。

**办法：** `ulimit -n 4096`；检查是否泄漏 fd（本步每连接都会 close，正常不会泄漏）。

### Q3：并发时偶尔 400 Bad Request

**原因：** 本步只 `read` 一次，TCP 分包时可能只收到半行请求行。

**说明：** 这是已知局限。Step6 的状态机会用 `read_buf_` 累积数据，彻底解决半包问题。Step4 本机 curl 小请求通常「碰巧能跑」。

### Q4：`bind 失败`

同 Step1：端口被占用。确认 `SO_REUSEADDR` 已设置；`ss -tlnp | grep 8080` 查占用。

### Q5：静态文件 404

**原因：** 未在项目根目录运行 `./server`（`www/` 是相对路径）。

**办法：** 一定在 `MyWebServer/` 根目录启动，或修改 `g_cfg.web_root` 为绝对路径。

### Q6：编译报 `epoll.h: No such file`

你在 Windows 原生环境编译了。请切换到 **WSL** 或 Linux 虚拟机。

---

## 第九部分：与单文件 Step4.cpp 对照

| 单文件 `Step4.cpp` | 分文件本步 | 说明 |
|-------------------|-----------|------|
| `const int PORT = 8080` | `g_cfg.port` | 配置集中到 `ServerConfig` |
| `const int MAX_EVENTS = 64` | `g_cfg.max_events` | 同上 |
| `setNonBlocking` / `addFd` / `removeFd` | `EpollHelper.cpp` | 从 main 拆出 |
| `handleClient` | `HttpConnection.cpp` | 从 main 拆出 |
| `parseRequestLine` 等 | `HttpParser` / `HttpResponse` / `HttpHandler` | Step2/3 已拆好 |
| `main` 里 epoll 循环 | `main.cpp` | 只保留 I/O 调度 |
| 无 `SO_REUSEADDR` | 有 | 分文件版补上，开发更稳 |
| 无 `modFd` | 有（暂未调用） | 为 Step8 预留 |

逻辑上与单文件 **等价**：同样是主线程 `epoll_wait` + 同步 `handleClient`。

---

## 第十部分：本篇小结与下一篇预告

### 你现在已经会了什么

- [x] 用 `epoll_create1` / `epoll_ctl` / `epoll_wait` 监视多个 fd
- [x] 非阻塞 `accept` 循环（accept 风暴）
- [x] 把 epoll 工具函数拆到 `EpollHelper` 模块
- [x] 把「处理一个客户端」拆到 `HttpConnection` 模块
- [x] `ServerConfig` 管理 `port` 和 `max_events`

### 本步尚未解决

| 局限 | 哪一步解决 |
|------|-----------|
| 业务逻辑阻塞主线程（读大文件时别的 fd 饿死） | Step5 线程池 |
| 只解析请求行，不支持 POST | Step6 状态机 |
| 只 read 一次，半包会出错 | Step6 `read_buf_` |
| write 阻塞主线程 | Step5 改到工作线程 |

### 系列进度

```text
Step1 阻塞 Hello World
    → Step2 解析请求行 + 拆模块
    → Step3 静态文件 www/
    → Step4 epoll 多连接  ← 你在这里
    → Step5 线程池
    → Step6 HTTP 状态机 + POST /echo
```

### 建议你自己做的两个小实验

1. 把 `g_cfg.max_events` 改成 `2`，同时发 5 个 curl——观察 `epoll_wait` 如何分批返回事件。
2. 在 `handleClient` 里 `sleep(3)` 再 write——体会「主线程同步处理」的瓶颈，为 Step5 线程池做铺垫。

---

> **下一步：** [05-Step5-ThreadPool](./05-Step5-ThreadPool.md) —— 引入线程池，主线程只管 I/O，工作线程拼响应。
