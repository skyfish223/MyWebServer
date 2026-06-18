# epoll 高并发 —— 零基础学习指南（第四篇）

> **写给谁看：** 已完成 [第三篇](./03-返回静态文件指南.md) 的学习者——静态文件 `www/` 能正常访问。  
> **本篇目标：** 用 **epoll** 让一个进程同时「盯住」很多连接，高效处理并发请求。  
> **不做的事：** 不用线程池、不做完整 HTTP 长连接 —— 这些留到第五篇。  
> **前置文档：** [01](./01-最小可行Web服务器指南.md) · [02](./02-解析HTTP请求指南.md) · [03](./03-返回静态文件指南.md)

---

## 目录

1. [第一部分：先搞懂"我们在升级什么"](#第一部分先搞懂我们在升级什么)
2. [第二部分：需要哪些新"工具"，为什么要用](#第二部分需要哪些新工具为什么要用)
3. [第三部分：完整代码 + 逐行讲解](#第三部分完整代码--逐行讲解)
4. [编译、运行与测试](#编译运行与测试)
5. [常见问题排查](#常见问题排查)
6. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

---

## 第一部分：先搞懂"我们在升级什么"

### 1.1 第三篇的瓶颈在哪？

第三篇的主循环本质上是：

```text
while (true) {
    accept 一个客人          ← 没客人时，程序卡在这里
    read → 处理 → write
    close
    再 accept 下一个       ← 前一个没处理完，后面的在排队
}
```

| 问题 | 说明 |
|------|------|
| **串行** | 同一时刻只为**一个**连接做 read/write |
| **accept 阻塞** | 干别的事时也在等「新客人」 |
| **客人多了会堵** | 处理慢时，后面连接在内核队列里等 |

对小作业、自己浏览器测一下**够用**；但真实网站同时很多人访问，就需要 **I/O 多路复用**（本篇的 epoll）。

### 1.2 生活类比：一个服务员怎么盯多桌？

| 模式 | 类比 | 第三篇 |
|------|------|--------|
| **串行** | 服务员盯完 1 号桌才去看 2 号桌 | `accept` → 处理完 → 再 `accept` |
| **epoll** | 服务员站在大厅中央，**哪桌举手（有数据）就去哪桌** | `epoll_wait` 一次返回多个「就绪」的 fd |

### 1.3 什么是 epoll？

**epoll** 是 Linux 提供的 **I/O 多路复用** 机制（只在 Linux / WSL 下用）。

你可以把它想成：

```text
一个「监视器」+ 一张「监视名单」
名单里有很多 fd（监听 socket、各个客户端 socket）
哪个 fd 上有事（新连接、有数据可读），epoll_wait 就告诉你
```

**一个进程、一个线程**，也能同时处理**很多连接**——不靠开很多线程。

### 1.4 epoll 的三个核心 API

| API | 一句话 |
|-----|--------|
| `epoll_create1` | 创建 epoll 实例（空监视器） |
| `epoll_ctl` | 往监视名单 **添加 / 修改 / 删除** 某个 fd |
| `epoll_wait` | **阻塞等待**，直到名单里至少有一个 fd「就绪」 |

### 1.5 本篇程序的整体流程

```text
  socket → bind → listen（和第三篇一样）
     │
     ▼
  epoll_create1
     │
     ▼
  epoll_ctl：把 listen_fd 加进 epoll（关心「新连接」）
     │
     ▼
  while (true) {
        epoll_wait  ← 等事件
        for 每个就绪的 fd {
            若是 listen_fd  → accept，把新 client_fd 也 epoll_ctl 加进去
            若是 client_fd   → read → 解析 → 读静态文件 → write → close → 从 epoll 删除
        }
     }
```

**业务逻辑（解析 HTTP、读 www）和第三篇相同**，变的只是 **「等事件、分发连接」** 的方式。

### 1.6 水平触发（LT）—— 本篇用的模式

epoll 有 **LT（水平触发）** 和 **ET（边缘触发）** 两种。

| 模式 | 通俗理解 | 本篇 |
|------|---------|------|
| **LT** | 数据还在，就会一直通知你 | ✅ 默认，好学 |
| **ET** | 只在「状态变化」时通知一次，编程更难 | 先不碰 |

本篇 **不加 `EPOLLET`**，就是 LT。配合「读一次、响应、关连接」足够学习。

---

## 第二部分：需要哪些新"工具"，为什么要用

### 2.1 新增头文件

| 头文件 | 提供什么 | 为什么需要 |
|--------|---------|-----------|
| `<sys/epoll.h>` | `epoll_create1`、`epoll_ctl`、`epoll_wait` | epoll 核心 API |
| `<fcntl.h>` | `fcntl`、`O_NONBLOCK` | 把 socket 设成**非阻塞**（epoll 推荐） |

第三篇的 `<fstream>`、`<string>` 等**全部保留**。

### 2.2 新增常量与结构体

| 名字 | 含义 |
|------|------|
| `MAX_EVENTS` | 一次 `epoll_wait` 最多返回多少个事件（如 64） |
| `epoll_event` | 描述「哪个 fd」以及「发生了什么事件」 |
| `EPOLLIN` | 可读（对新连接 = 可以 `accept`；对客户端 = 可以 `read`） |
| `EPOLL_CTL_ADD` | 把 fd 加入 epoll 监视 |
| `EPOLL_CTL_DEL` | 从 epoll 监视中移除 |

### 2.3 新增辅助函数

| 函数 | 职责 |
|------|------|
| `setNonBlocking(fd)` | `fcntl` 给 fd 加上 `O_NONBLOCK` |
| `addFd(epfd, fd)` | 封装 `epoll_ctl(ADD, fd, EPOLLIN)` |
| `buildResponseFromRequest(req)` | 根据 `HttpRequest` 生成 HTTP 响应字符串（**第三篇 `handleRequest` 同款逻辑**） |
| `handleClient(epfd, fd)` | 对单个客户端：read → 解析 → 调上面函数 → write → 从 epoll 删除 → close |

### 2.4 为什么把 `handleRequest` 改成 `buildResponseFromRequest`？

第三篇里，处理一次 HTTP 请求的核心函数叫 **`handleRequest`**：

```cpp
string handleRequest(const HttpRequest& req) {
    // GET 检查 → 路径安全 → 读 www 文件 → buildHttpResponse
}
```

第四篇**没有删掉这段逻辑**，只是换了个名字。很多人会问：是不是多了一个新函数？其实 **就是同一个角色**。

#### 为什么要改名？

第四篇又多了一个函数 **`handleClient`**，两个名字都带 handle，很容易搞混：

| 函数 | 管什么 | 和 socket / epoll 的关系 |
|------|--------|-------------------------|
| **`handleClient`** | 处理**某一个客户端 fd** 的完整流程 | 要 `read`、`write`、`close`，还要 `epoll_ctl DEL` |
| **`buildResponseFromRequest`**（原 `handleRequest`） | 只根据 **HttpRequest** 拼出响应字符串 | **不碰** fd，不知道 epoll 存在 |

```text
第三篇：
  main 里 accept → read → parseRequestLine → handleRequest(req) → write → close
  （一条线做完，函数名叫 handleRequest 很贴切）

第四篇：
  epoll 回调 → handleClient(fd)
                  ├─ read / parseRequestLine   ← I/O 层
                  ├─ buildResponseFromRequest  ← 纯业务：req → response 字符串
                  └─ write / removeFd / close ← I/O 层
```

**生活类比：**

- **`buildResponseFromRequest`**：厨房按菜单**做菜**（和第三篇一样）
- **`handleClient`**：服务员**端盘子**：从客人桌上听需求、把菜端上桌、收拾桌子（还涉及 epoll「这桌还要不要盯」）

改名是为了让人一眼看出：**这个函数只负责「从请求结构体生成响应正文」**，不负责网络读写。

#### 必须改名吗？

**不必须。** 你完全可以继续叫 `handleRequest`，在 `handleClient` 里写：

```cpp
response = handleRequest(req);   // 和第三篇同名，完全合法
```

教程用 `buildResponseFromRequest` 只是为了**分工更清晰**。以后分文件时，也可以把静态文件相关函数都放进 `HttpStatic.cpp`，名字随你习惯。

#### 和第三篇对照

| 第三篇 | 第四篇 | 逻辑是否相同 |
|--------|--------|-------------|
| `handleRequest(req)` | `buildResponseFromRequest(req)` | ✅ 相同（GET/403/404/读文件/MIME） |
| `main` 里直接 read/write | `handleClient` 里 read/write | 只是拆了一层 |

---

### 2.5 为什么要非阻塞？

| 阻塞 socket | 非阻塞 socket |
|------------|--------------|
| `read` 没数据会一直睡 | 没数据立刻返回 `-1`，`errno == EAGAIN` |
| 容易把线程卡死 | 配合 epoll：**有 EPOLLIN 再去 read** |

本篇逻辑：**epoll 通知可读 → 再 read 一次 → 处理完就 close**，和第三篇一样简单。

### 2.6 和 select / poll 的对比（了解即可）

| 方式 | 一句话 |
|------|--------|
| `select` | 古老，fd 数量有限，每次要遍历整个集合 |
| `poll` | 改进，但仍要遍历 |
| **`epoll`** | Linux 高并发常用，活跃连接多时有优势 |

你写的 MyWebServer 目标就是 **Linux + epoll**，和很多开源 Web 服务器路线一致。

---

## 第三部分：完整代码 + 逐行讲解

### 3.1 完整代码

保存为 **`Step1to6/Step4.cpp`**（在 `Step1to6/Step3.cpp` 基础上改 `main` 和少量封装）：

```cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

const string WEB_ROOT = "www";
const int MAX_EVENTS = 64;
const int PORT = 8080;

struct HttpRequest {
    string method;
    string path;
    string version;
};

// ========== 第三篇：HTTP + 静态文件（保持不变）==========
bool parseRequestLine(const string& raw, HttpRequest& req) {
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == string::npos) lineEnd = raw.find('\n');
    if (lineEnd == string::npos) return false;
    string line = raw.substr(0, lineEnd);
    istringstream iss(line);
    if (!(iss >> req.method >> req.path >> req.version)) return false;
    return true;
}

string buildHttpResponse(int statusCode, const string& statusText,
                         const string& contentType, const string& body) {
    return "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html")
        return WEB_ROOT + "/index.html";
    return WEB_ROOT + urlPath;
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
    out.assign((istreambuf_iterator<char>(ifs)),
               istreambuf_iterator<char>());
    return true;
}

string buildResponseFromRequest(const HttpRequest& req) {
    if (req.method != "GET") {
        return buildHttpResponse(405, "Method Not Allowed",
            "text/plain; charset=utf-8", "暂只支持 GET");
    }
    if (!isPathSafe(req.path)) {
        return buildHttpResponse(403, "Forbidden",
            "text/plain; charset=utf-8", "403 - 非法路径");
    }
    string filePath = pathToFile(req.path);
    string body;
    if (!readFile(filePath, body)) {
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "404 - 文件不存在: " + req.path);
    }
    return buildHttpResponse(200, "OK", getContentType(filePath), body);
}

// ↑ 即第三篇的 handleRequest，改名原因见第二部分 2.4 节

// ========== 第四篇新增：epoll 工具函数 ==========
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void addFd(int epfd, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;   // 关心「可读」（LT 默认）
    ev.data.fd = fd;       // 就绪时知道是哪个 fd
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void removeFd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

// 处理一个客户端连接（第三篇逻辑的封装）
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
            cout << "[fd " << client_fd << "] " << req.method << " " << req.path << "\n";
            response = buildResponseFromRequest(req);
        }
    }

    write(client_fd, response.c_str(), response.size());
    removeFd(epfd, client_fd);
    close(client_fd);
}

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
    addr.sin_port = htons(PORT);

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

    cout << "epoll 服务器已启动：http://127.0.0.1:" << PORT << "\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";

    vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        if (nready < 0) {
            cerr << "epoll_wait 失败\n";
            continue;
        }

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // 监听 socket 就绪 = 有新连接
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           (sockaddr*)&client_addr, &len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;  // 新连接接受完了
                        cerr << "accept 失败\n";
                        break;
                    }
                    setNonBlocking(client_fd);
                    addFd(epfd, client_fd);
                    cout << "新连接 fd=" << client_fd << "\n";
                }
            } else {
                // 客户端 socket 就绪 = 可以 read
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

### 3.2 逐块讲解：epoll 部分

#### `buildResponseFromRequest` —— 就是第三篇的 `handleRequest`

函数体与第三篇 `handleRequest` 一致：检查 GET → `isPathSafe` → `pathToFile` → `readFile` → `getContentType` → `buildHttpResponse`。

**为何文档里换名？** 见 [2.4 节](#24-为什么把-handlerequest-改成-buildresponsefromrequest)：第四篇另有 `handleClient` 负责 read/write/epoll，为避免两个 `handle*` 混淆，把「纯业务、只生成响应字符串」的函数改叫 `buildResponseFromRequest`。你继续用 `handleRequest` 也可以。

---

#### `setNonBlocking`

```cpp
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

把 fd 设成**非阻塞**。  
`accept` / `read` 在无数据时不会卡死整个进程，而是返回 `EAGAIN`。

---

#### `addFd` / `removeFd`

```cpp
epoll_event ev{};
ev.events = EPOLLIN;    // 监听「可读」
ev.data.fd = fd;        // 回调时知道是哪个 fd
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
```

| 字段 | 含义 |
|------|------|
| `EPOLLIN` | 对 listen_fd：可以 `accept`；对 client_fd：可以 `read` |
| `ev.data.fd` | 把 fd 存进去，`epoll_wait` 返回时原样给你 |

处理完客户端后 **`EPOLL_CTL_DEL` + `close`**，避免监视列表泄漏。

---

#### `epoll_wait`

```cpp
int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
```

| 参数 | 含义 |
|------|------|
| `epfd` | epoll 实例 |
| `events.data()` | 输出数组：哪些 fd 就绪了 |
| `MAX_EVENTS` | 数组最多装几个事件 |
| `-1` | 一直等，直到有事件（阻塞） |

返回 `nready`：本次有几个 fd 就绪。

---

#### 分支一：`fd == listen_fd`（新连接）

```cpp
while (true) {
    int client_fd = accept(...);
    if (client_fd < 0 && errno == EAGAIN) break;
    setNonBlocking(client_fd);
    addFd(epfd, client_fd);
}
```

**为什么 `accept` 用 while？**  
一次通知可能对应**多个**新连接排队，要循环 `accept` 直到 `EAGAIN`。

**生活类比：** 门口铃响了，可能不止一位客人同时进来，要挨个接待登记。

---

#### 分支二：客户端 fd（HTTP 请求）

```cpp
handleClient(epfd, fd);
// read → 解析 → 静态文件 → write → removeFd → close
```

和第三篇**业务相同**，只是触发方式从「accept 之后立刻 read」变成「epoll 说可读再 read」。

---

### 3.3 与第三篇对照表

| 项目 | 第三篇 | 第四篇 |
|------|--------|--------|
| 等待连接 | `accept` 阻塞 | `epoll_wait` + listen_fd 事件 |
| 等待数据 | `accept` 后立刻 `read` | client_fd 的 `EPOLLIN` 再 `read` |
| 并发 | 串行处理 | 单进程监视多 fd，轮流处理就绪的 |
| 静态文件业务 | `handleRequest` | `buildResponseFromRequest`（**逻辑相同**，见 2.4 节） |
| 单连接 I/O 封装 | 无（在 `main` 里写） | `handleClient` |
| 连接结束 | `close` | `close` + `epoll_ctl DEL` |

---

## 编译、运行与测试

### 4.1 环境

- **Linux 或 WSL2**（epoll 是 Linux 专用，Windows 原生不支持）
- 确保 `www/` 目录存在（第三篇已建）

### 4.2 编译

在**项目根目录**执行：

```bash
g++ -std=c++17 -Wall -o server Step1to6/Step4.cpp
```

### 4.3 运行

仍在项目根目录：

```bash
./server
```

应看到：

```text
epoll 服务器已启动：http://127.0.0.1:8080
网站根目录: www/
```

### 4.4 浏览器测试

访问 `http://127.0.0.1:8080/`，页面应与第三篇一样正常（含 CSS）。

终端可能交错打印：

```text
新连接 fd=5
[fd 5] GET /
新连接 fd=6
[fd 6] GET /css/style.css
```

说明 **两个连接被 epoll 分别处理**。

### 4.5 简单并发测试（可选）

另开终端，多请求几次：

```bash
curl -s http://127.0.0.1:8080/ > /dev/null &
curl -s http://127.0.0.1:8080/about.html > /dev/null &
curl -s http://127.0.0.1:8080/ > /dev/null &
wait
```

服务器应都能响应，不会只能处理一个。

### 4.6 成功现象 checklist

| 检查项 | 预期 |
|--------|------|
| 首页 + 样式 | 与 Step3 一致 |
| 终端 | 出现多个不同 `fd` |
| 404 | 访问不存在文件仍返回 404 |
| 无 epoll 报错 | 没有反复 `epoll_wait 失败` |

---

## 常见问题排查

### Q1：编译报错 `sys/epoll.h: No such file`

不在 Linux/WSL。请在 WSL 或 Linux 虚拟机里编译运行。

### Q2：`epoll_create1` / `epoll_wait` 失败

看 `errno`；常见是资源耗尽或参数错误。确认 `MAX_EVENTS > 0`。

### Q3：浏览器连不上，没有任何「新连接」日志

- `bind` 失败：端口占用  
- 防火墙  
- 没在项目根目录运行（不影响 bind，但会影响静态文件）

### Q4：第一个请求正常，之后全挂

可能 **client_fd 处理完没有 `EPOLL_CTL_DEL`**，或重复 `close` 同一 fd。对照 `handleClient` 末尾四行。

### Q5：`accept` 只进来一个连接

listen 没设非阻塞、或没 `while` 读到 `EAGAIN`。对照 3.2 监听分支。

### Q6：和第三篇比感觉差不多快？

本地只有自己访问时差别不大；epoll 的优势在**大量连接同时挂着**时更明显。可用上面的多 `curl` 后台命令试。

### Q7：想继续用 `Httpparase.h` 分文件

可以。把 `parseRequestLine`、`buildHttpResponse`、`buildResponseFromRequest` 等放进 `.h/.cpp`，`Step4.cpp` 的 `main` 只保留 epoll 循环，编译：

```bash
g++ -std=c++17 -Wall -o server Step1to6/Step4.cpp Httpparase.cpp
```

---

## 本篇小结与下一篇预告

### 你现在已经会了什么

- [x] 理解第三篇 **串行阻塞** 的局限
- [x] 使用 **`epoll_create1` / `epoll_ctl` / `epoll_wait`** 监视多个 fd
- [x] **listen_fd** 与新 **client_fd** 分开处理
- [x] **`fcntl` 非阻塞** + `accept` 循环读到 `EAGAIN`
- [x] 在 epoll 循环里**嵌入第三篇静态文件逻辑**
- [x] 处理完连接后 **`DEL` + `close`**

### 和后续篇章的关系

```text
第一篇 socket
    ▼
第二篇 解析 HTTP
    ▼
第三篇 静态文件 www/
    ▼
本篇 epoll 单进程多连接
    ▼
第五篇 线程池（把耗时工作交给工作线程）
```

### 建议你自己做的三个小实验

1. 在 `handleClient` 里 `sleep(2)` 再响应，同时开两个浏览器标签访问——观察 epoll 如何交替处理（体会仍串行，但**不会卡在 accept**）。
2. 打印每次 `epoll_wait` 的 `nready`——看一次唤醒处理几个 fd。
3. 查 `man epoll`，浏览 `EPOLLIN`、`EPOLLOUT`、`EPOLLET` 说明——为以后进阶打基础。

---

> **下一步：** 进入 [第五篇：线程池](./05-线程池指南.md)，在 epoll 基础上把「读文件、拼响应」放到工作线程，进一步利用多核 CPU。
