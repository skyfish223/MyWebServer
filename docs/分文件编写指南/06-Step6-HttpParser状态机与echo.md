# 分文件 Step6 —— HttpParser 状态机与 POST /echo

> **写给谁看：** 已完成 [Step5 分文件版](./05-Step5-ThreadPool.md) 的学习者——epoll + 线程池能正常返回 GET 静态文件。  
> **对应单文件：** [Step1to6/step6.cpp](../../Step1to6/step6.cpp) · [06-HTTP状态机指南](../06-HTTP状态机指南.md)  
> **本篇目标：** 三态解析（请求行 → Header → Body）；支持 POST `/echo`；GET `/echo` 返回表单页。  
> **本步主要修改：** `HttpTypes`、`HttpParser`、`HttpResponse`、`HttpHandler`、`HttpConnection`、`ServerConfig`、`main`

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
9. [第九部分：与单文件 step6.cpp 对照](#第九部分与单文件-step6cpp-对照)
10. [第十部分：本篇小结与下一篇预告](#第十部分本篇小结与下一篇预告)

---

## 第一部分：架构说明——我们在升级什么

### 1.1 Step5 的瓶颈

Step5 里，主线程 `read` 一次就把整个 buffer 当字符串，然后 `parseRequestLine` 解析**第一行**：

```text
read 一次
  → parseRequestLine（只认 GET /path HTTP/1.1）
  → handleRequest（静态文件）
  → write + close
```

| 问题 | 说明 |
|------|------|
| **只认请求行** | Header（如 `Content-Length`）和 Body 被忽略 |
| **不支持 POST** | 表单数据在 Body 里，Step5 永远拿不到 |
| **假定一次 read 收齐** | TCP 可能分包：第一次只收到半行、半个 Header |
| **read 完就 removeFd** | 数据没收齐时连接被关掉，无法「等下次再读」 |

### 1.2 生活类比：办手续要分三步

```text
状态 A：填表（请求行）     →  METHOD /path HTTP/1.1
   ↓ 表填完
状态 B：交材料（Header）   →  Host: ...、Content-Length: 27 ...
   ↓ 材料齐；空行表示 Header 结束
状态 C：交正文（Body）     →  username=abc&passwd=123456
   ↓
办结：可以生成 HTTP 响应
```

Step2/Step5 的 `parseRequestLine` 等价于**只做了状态 A**。  
浏览器 POST 时，正文在 **Body**；Body 多长由 Header 里的 **`Content-Length`** 声明。不解析 Header，就无法正确读 Body。

### 1.3 HTTP 请求在字节流里长什么样

```http
POST /echo HTTP/1.1
Host: 127.0.0.1:8080
Content-Type: application/x-www-form-urlencoded
Content-Length: 11

hello=world
```

结构：

```text
┌─────────────────────────────────────┐
│  请求行                              │  POST /echo HTTP/1.1
├─────────────────────────────────────┤
│  Header（多行，Key: Value）           │  Host: ...
│                                     │  Content-Length: 11
├─────────────────────────────────────┤
│  空行 \r\n                           │  （Header 结束标志）
├─────────────────────────────────────┤
│  Body（POST 才有，长度 = Content-Length）│  hello=world
└─────────────────────────────────────┘
```

**GET** 通常没有 Body：Header 后的空行出现 → 请求完整，可以处理。  
**POST** 有 Body：空行出现后还要再收 `Content-Length` 个字节。

### 1.4 本篇架构变化

```text
┌──────────────────────────────────────────────────────────────────┐
│  main：epoll_wait + connections[fd] → HttpConnection              │
└────────────────────────────┬─────────────────────────────────────┘
                             │ EPOLLIN
                             ▼
┌──────────────────────────────────────────────────────────────────┐
│  handle_read（主线程）                                            │
│    append_read  → 内核字节写入 conn.read_buf_                     │
│    process_read → 状态机解析，返回 Incomplete / Complete / Error  │
│    Incomplete   → return，保持 epoll 监听，等下次 read            │
│    Complete     → removeFd + submit 线程池 do_request + write     │
│    Error        → closeConnection                                 │
└──────────────────────────────────────────────────────────────────┘
```

### 1.5 分文件后的模块职责

| 模块 | 本步变化 |
|------|---------|
| `HttpTypes` | 增加 `headers`、`body`、`content_length`；`ParseState`、`ReadResult` |
| `HttpParser` | `get_line`、`process_read`、`append_read` 状态机核心 |
| `HttpHandler` | `do_request` 统一入口；`buildPostEchoResponse` |
| `HttpResponse` | `pathToFile` 增加 `/echo` → `echo.html` |
| `HttpConnection` | 升级为类：`state_`、`request_`、`read_buf_`；`handle_read` |
| `ServerConfig` | `max_body_size` 限制 Body 上限 |
| `EpollHelper` | 增加 `closeConnection` |
| `main` | `unordered_map<int, HttpConnection> connections` |

### 1.6 三个主状态

| 状态 | 枚举名 | 在干什么 |
|------|--------|---------|
| 解析请求行 | `ParseState::RequestLine` | 读第一行 `GET / HTTP/1.1` |
| 解析 Header | `ParseState::Header` | 逐行读 `Key: Value`，直到空行 |
| 读 Body | `ParseState::Content` | 按 `Content-Length` 从 `read_buf_` 抠出正文 |

---

## 第二部分：需要哪些新工具

### 2.1 新增头文件 / 类型

| 头文件 / 类型 | 用途 |
|-------------|------|
| `<map>` | `HttpRequest::headers` |
| `<unordered_map>` | `connections` 映射表 |
| `<algorithm>` + `<cctype>` | `trim`、`toLower` 处理 Header |
| `enum class ParseState` | 解析阶段 |
| `enum class ReadResult` | `append_read` + `process_read` 的返回值 |

### 2.2 两个核心函数分工

| 函数 | 方向 | 职责 |
|------|------|------|
| `append_read(conn)` | 内核 → `read_buf_` | 非阻塞 `read`，把原始字节追加到缓冲区 |
| `process_read(conn)` | `read_buf_` → `request_` | 状态机：按行解析、按长度抠 Body |

**为什么要分开？**  
`read` 是 I/O 操作，可能一次只收到几个字节；`process_read` 是纯内存解析，可以反复调用直到 `Incomplete`（数据不够）或 `Complete`（请求完整）。

### 2.3 `get_line` 与 `\r\n`

HTTP 规定行结束符是 `\r\n`。`get_line` 从 `read_buf_` 里找 `\r\n`，取出一行并从 buffer 删掉（含 `\r\n` 两个字符）。

若 buffer 里没有完整一行，返回 `false`，`process_read` 返回 `Incomplete`——等下次 `append_read` 再来数据。

---

## 第三部分：完整文件树

```text
MyWebServer/
├── Makefile
├── src/
│   ├── main.cpp                 ← 修改：connections map
│   ├── ServerConfig.h           ← 扩展 max_body_size
│   ├── ServerConfig.cpp
│   ├── HttpTypes.h              ← 大改
│   ├── HttpParser.h             ← 大改
│   ├── HttpParser.cpp           ← 大改
│   ├── HttpResponse.h
│   ├── HttpResponse.cpp         ← pathToFile 增加 /echo
│   ├── HttpHandler.h            ← 增加 do_request
│   ├── HttpHandler.cpp          ← 大改
│   ├── EpollHelper.h            ← 增加 closeConnection
│   ├── EpollHelper.cpp          ← 增加 closeConnection
│   ├── HttpConnection.h         ← 升级为类
│   ├── HttpConnection.cpp       ← handle_read
│   ├── ThreadPool.h
│   └── ThreadPool.cpp
└── www/
    ├── index.html
    ├── echo.html                ← POST 测试表单页
    └── ...
```

---

## 第四部分：完整源码（逐步粘贴）

### 4.1 `src/ServerConfig.h`

```cpp
#pragma once
#include <string>
#include <cstddef>

struct ServerConfig {
    std::string web_root = "www";
    int port = 8080;
    int max_events = 64;
    std::size_t thread_pool_size = 4;
    std::size_t max_body_size = 1024 * 1024;
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
#include <map>
#include <cstddef>

enum class ParseState {
    RequestLine,
    Header,
    Content
};

enum class ReadResult {
    Incomplete,
    Complete,
    Error
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::size_t content_length = 0;
    std::string body;
};
```

### 4.4 `src/HttpConnection.h`

```cpp
#pragma once
#include "HttpTypes.h"
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
};

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 std::unordered_map<int, HttpConnection>& connections);
```

### 4.5 `src/HttpParser.h`

```cpp
#pragma once
#include "HttpTypes.h"
#include <string>

class HttpConnection;

bool get_line(std::string& buf, std::string& line);
bool parse_request_line(const std::string& line, HttpRequest& req);
void parse_header_line(const std::string& line, HttpRequest& req);
ReadResult process_read(HttpConnection& conn);
bool append_read(HttpConnection& conn);
```

### 4.6 `src/HttpParser.cpp`

从 [step6.cpp](../../Step1to6/step6.cpp) 迁入，使用 `g_cfg.max_body_size`：

```cpp
#include "HttpParser.h"
#include "HttpConnection.h"
#include "ServerConfig.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <cerrno>

using namespace std;

static string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static string toLower(string s) {
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool get_line(string& buf, string& line) {
    size_t pos = buf.find("\r\n");
    if (pos == string::npos) return false;
    line = buf.substr(0, pos);
    buf.erase(0, pos + 2);
    return true;
}

bool parse_request_line(const string& line, HttpRequest& req) {
    istringstream iss(line);
    return static_cast<bool>(iss >> req.method >> req.path >> req.version);
}

void parse_header_line(const string& line, HttpRequest& req) {
    size_t colon = line.find(':');
    if (colon == string::npos) return;
    string key = toLower(trim(line.substr(0, colon)));
    string val = trim(line.substr(colon + 1));
    req.headers[key] = val;
    if (key == "content-length")
        req.content_length = static_cast<size_t>(stoul(val));
}

ReadResult process_read(HttpConnection& conn) {
    string line;
    while (true) {
        if (conn.state_ == ParseState::Content) {
            if (conn.request_.content_length > g_cfg.max_body_size)
                return ReadResult::Error;
            if (conn.read_buf_.size() >= conn.request_.content_length) {
                conn.request_.body = conn.read_buf_.substr(0, conn.request_.content_length);
                conn.read_buf_.erase(0, conn.request_.content_length);
                return ReadResult::Complete;
            }
            return ReadResult::Incomplete;
        }

        if (!get_line(conn.read_buf_, line))
            return ReadResult::Incomplete;

        switch (conn.state_) {
            case ParseState::RequestLine:
                if (!parse_request_line(line, conn.request_))
                    return ReadResult::Error;
                conn.state_ = ParseState::Header;
                break;

            case ParseState::Header:
                if (line.empty()) {
                    if (conn.request_.method == "POST" && conn.request_.content_length > 0)
                        conn.state_ = ParseState::Content;
                    else
                        return ReadResult::Complete;
                } else {
                    parse_header_line(line, conn.request_);
                }
                break;

            case ParseState::Content:
                break;
        }
    }
}

bool append_read(HttpConnection& conn) {
    char buffer[8192];
    ssize_t n = read(conn.fd_, buffer, sizeof(buffer));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    if (n == 0) return false;
    conn.read_buf_.append(buffer, static_cast<size_t>(n));
    return true;
}
```

### 4.7 `src/HttpResponse.h`（不变）

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

### 4.8 `src/HttpResponse.cpp`（pathToFile 增加 /echo）

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
    if (urlPath == "/echo" || urlPath == "/echo.html")
        return g_cfg.web_root + "/echo.html";
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

### 4.9 `src/HttpHandler.h`

```cpp
#pragma once
#include "HttpTypes.h"
#include <string>

std::string handleRequest(const HttpRequest& req);
std::string do_request(const HttpRequest& req);
```

### 4.10 `src/HttpHandler.cpp`

```cpp
#include "HttpHandler.h"
#include "HttpResponse.h"

using namespace std;

static string buildGetResponse(const HttpRequest& req) {
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

static string buildPostEchoResponse(const HttpRequest& req) {
    string msg = "收到 POST body:\n" + req.body;
    return buildHttpResponse(200, "OK", "text/plain; charset=utf-8", msg);
}

string handleRequest(const HttpRequest& req) {
    return buildGetResponse(req);
}

string do_request(const HttpRequest& req) {
    if (req.method == "GET")
        return buildGetResponse(req);
    if (req.method == "POST") {
        if (req.path == "/echo")
            return buildPostEchoResponse(req);
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "未知 POST 路径: " + req.path);
    }
    return buildHttpResponse(405, "Method Not Allowed",
        "text/plain; charset=utf-8", "暂只支持 GET 和 POST /echo");
}
```

### 4.11 `src/EpollHelper.h`（增加 closeConnection）

```cpp
#pragma once
#include <cstdint>
#include <unordered_map>

class HttpConnection;

void setNonBlocking(int fd);
void addFd(int epfd, int fd, uint32_t events = 0x001);
void modFd(int epfd, int fd, uint32_t events);
void removeFd(int epfd, int fd);
void closeConnection(int epfd, int fd,
                     std::unordered_map<int, HttpConnection>& connections);
```

### 4.12 `src/EpollHelper.cpp`

```cpp
#include "EpollHelper.h"
#include "HttpConnection.h"
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

> **Step7 预告：** Step6 把 `closeConnection` 放在 `EpollHelper` 是为了先跑通状态机。进入 [Step7](./07-Step7-定时器.md) 时，要把这里的 **声明和实现删掉**，改在 `HttpConnection.cpp` 里实现（带 `find` 防重复 close）。**不要两个文件各写一份**，否则链接报 `multiple definition of closeConnection`。

### 4.13 `src/HttpConnection.cpp`

```cpp
#include "HttpConnection.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "EpollHelper.h"
#include "ThreadPool.h"
#include <iostream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

using namespace std;

HttpConnection::HttpConnection(int fd) : fd_(fd) {}

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& connections) {
    if (!append_read(conn)) {
        cout << "[main] 读失败或连接关闭 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, connections);
        return;
    }

    ReadResult result = process_read(conn);

    if (result == ReadResult::Incomplete)
        return;

    if (result == ReadResult::Error) {
        cout << "[main] 解析错误 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, connections);
        return;
    }

    HttpRequest req = move(conn.request_);
    int client_fd = conn.fd_;
    removeFd(epfd, client_fd);
    connections.erase(client_fd);

    pool.submit([client_fd, req]() {
        cout << "[worker " << this_thread::get_id() << "] "
             << req.method << " " << req.path;
        if (!req.body.empty()) cout << " body_len=" << req.body.size();
        cout << "\n";

        string response = do_request(req);
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "[worker] 完成 fd=" << client_fd << "\n";
    });
}
```

### 4.14 `src/ThreadPool.h`（与 Step5 相同）

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

### 4.15 `src/ThreadPool.cpp`（与 Step5 相同）

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

### 4.16 `src/main.cpp`

```cpp
#include <iostream>
#include <vector>
#include <unordered_map>
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

    cout << "Step6 状态机服务器已启动：http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "工作线程数: " << g_cfg.thread_pool_size << "\n";
    cout << "网站根目录: " << g_cfg.web_root << "/\n";
    cout << "POST 测试: curl -X POST http://127.0.0.1:" << g_cfg.port
         << "/echo -d 'a=b'\n";

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
                    connections.emplace(client_fd, HttpConnection(client_fd));
                    cout << "[main] 新连接 fd=" << client_fd << "\n";
                }
            } else {
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                handle_read(epfd, it->second, pool, connections);
            }
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
```

### 4.17 `www/echo.html`（确认存在）

仓库已自带。浏览器访问 `http://127.0.0.1:8080/echo` 会 GET 此表单页；提交表单会 POST 到 `/echo`。

---

## 第五部分：Makefile

与 Step5 相同（已含 `ThreadPool.cpp` 和 `-pthread`）：

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

### 6.1 `process_read` 状态机主循环

```text
while (true) {
    若 state == Content：
        检查 content_length 是否超限 → Error
        若 read_buf 够长 → 抠出 body → Complete
        否则 → Incomplete

    若 buffer 里没有完整一行 → Incomplete

    switch (state) {
        RequestLine: 解析第一行 → 转 Header
        Header:      非空行 → parse_header_line
                     空行   → POST 且有 body → Content；否则 Complete
    }
}
```

**空行的含义：** Header 区结束。GET 到此即可办结；POST 若 `content_length > 0` 进入 Content 状态继续等 Body。

### 6.2 POST 且无 Body 的边界

```cpp
if (conn.request_.method == "POST" && conn.request_.content_length > 0)
    conn.state_ = ParseState::Content;
else
    return ReadResult::Complete;
```

`POST` 但 `Content-Length: 0`（或没写）→ 直接 `Complete`，`do_request` 里 `req.body` 为空字符串。

### 6.3 `append_read` 与半包

```cpp
if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // 暂时没数据，不是错误
    return false;  // 真错误
}
if (n == 0) return false;  // 对端关闭
conn.read_buf_.append(buffer, n);
```

`handle_read` 里：

```text
append_read 成功
  → process_read 返回 Incomplete → return（fd 仍在 epoll 上，等下次 EPOLLIN）
  → process_read 返回 Complete  → submit 线程池
```

这就是 Step5「read 一次就关」做不到的事。

### 6.4 `connections` 映射表

```cpp
connections.emplace(client_fd, HttpConnection(client_fd));
// ...
auto it = connections.find(fd);
handle_read(epfd, it->second, pool, connections);
```

每个客户端 fd 对应一个 `HttpConnection` 对象，保存该连接的解析状态和读缓冲。  
请求完整后 `connections.erase(client_fd)`，对象销毁；fd 由工作线程 close。

### 6.5 `do_request` 路由

| 请求 | 处理 |
|------|------|
| `GET /` | `buildGetResponse` → 读 `www/index.html` |
| `GET /echo` | `pathToFile` 映射到 `www/echo.html` 表单页 |
| `POST /echo` | `buildPostEchoResponse` → 回显 body 纯文本 |
| `POST /other` | 404 |
| `DELETE ...` | 405 |

### 6.6 `max_body_size` 防护

```cpp
if (conn.request_.content_length > g_cfg.max_body_size)
    return ReadResult::Error;
```

防止恶意客户端声明 `Content-Length: 999999999` 撑爆内存。默认 1MB，与 [step6.cpp](../../Step1to6/step6.cpp) 一致。

### 6.7 完整数据流（POST /echo）

```text
浏览器表单 POST
    │
    ▼
内核 TCP 缓冲区（可能分多个包）
    │
    ▼ append_read（可能调用多次）
conn.read_buf_ = "POST /echo HTTP/1.1\r\nHost:...\r\n\r\nhello=world"
    │
    ▼ process_read
conn.request_.method = "POST"
conn.request_.path   = "/echo"
conn.request_.body   = "hello=world"
    │
    ▼ 线程池 do_request
"收到 POST body:\nhello=world"
    │
    ▼ write + close
浏览器显示回显文本
```

---

## 第七部分：编译、运行与验收

### 7.1 编译

```bash
make clean && make
```

### 7.2 运行

```bash
./server
```

### 7.3 GET 静态页

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/echo
```

第二条应返回 `www/echo.html` 的 HTML 表单。

### 7.4 POST /echo（curl）

```bash
curl -X POST http://127.0.0.1:8080/echo -d "a=b&msg=hello"
```

期望响应正文包含：

```text
收到 POST body:
a=b&msg=hello
```

### 7.5 浏览器测试（推荐）

1. 打开 `http://127.0.0.1:8080/echo`
2. 修改表单里的消息、备注
3. 点击「POST 到 /echo」
4. 页面应显示服务器回显的 body 内容

### 7.6 服务器日志

```text
[main] 新连接 fd=7
[worker 140728394837120] POST /echo body_len=11
[worker] 完成 fd=7
```

POST 请求应打印 `body_len=...`。

### 7.7 成功 checklist

| 测试 | 期望 |
|------|------|
| GET `/` | 200，首页 HTML |
| GET `/echo` | 200，表单页 HTML |
| POST `/echo -d "test"` | 200，正文含 `test` |
| 浏览器提交表单 | 回显填写的字段 |
| 超大 Content-Length | 连接被关闭，日志「解析错误」 |

---

## 第八部分：常见问题排查

### Q1：POST 返回空 body 或 400

**检查：**

1. 请求是否带 `Content-Length` Header（curl `-d` 会自动加）
2. `get_line` 是否用 `\r\n` 分行（只用 `\n` 会解析失败）
3. 浏览器表单 `method="post"` `action="/echo"` 是否正确

### Q2：GET /echo 返回 404

**原因：** `pathToFile` 未加 `/echo` 映射，或 `www/echo.html` 不存在。

**办法：** 对照本篇 4.8 节；确认在项目根目录运行 `./server`。

### Q3：请求一直 Incomplete，永不响应

**原因：** `Content-Length` 声明的长度大于实际收到的 body；或客户端没发完就断连。

**调试：** 在 `append_read` 后打印 `conn.read_buf_.size()` 和 `content_length`。

### Q4：`stoul` 异常 / 解析错误

**原因：** `Content-Length` 不是合法数字。

**说明：** 本步直接 `stoul`，非法值可能抛异常。生产代码应 try-catch；学习阶段够用。

### Q5：与 Step5 比变慢了？

**正常。** 连接要保持更久（等 Body 收齐），`connections` 对象更大。换来的是**正确性**。

### Q6：能否支持 chunked 编码？

本步**不支持** `Transfer-Encoding: chunked`，只认 `Content-Length`。chunked 是更后面的进阶话题。

### Q7：`handle_read` 里 `connections.erase` 后 it 失效？

`erase(client_fd)` 在 `removeFd` 之后，且不再使用 `it`，安全。`handle_read` 调用处若需继续用 `it`，应在调用前注意——本步 `handle_read` 末尾已 erase，主循环不会复用。

---

## 第九部分：与单文件 step6.cpp 对照

| 单文件 `step6.cpp` | 分文件本步 | 说明 |
|-------------------|-----------|------|
| `enum class ParseState` | `HttpTypes.h` | 类型集中 |
| `get_line` / `process_read` / `append_read` | `HttpParser.cpp` | 解析模块 |
| `class HttpConnection` | `HttpConnection.h` | 连接状态 |
| `handle_read` | `HttpConnection.cpp` | I/O + 调度 |
| `closeConnection` | `EpollHelper.cpp`（Step6）；Step7 起迁至 `HttpConnection.cpp` | 关闭辅助 |
| `do_request` / `buildPostEchoResponse` | `HttpHandler.cpp` | 业务路由 |
| `MAX_BODY_SIZE` 常量 | `g_cfg.max_body_size` | 配置化 |
| `parseRequestLine`（旧） | 被 `parse_request_line` 取代 | 状态机内使用 |
| `dispatchClient`（Step5） | 被 `handle_read` 取代 | 不再 read 一次就 submit |

逻辑与单文件 **等价**。分文件版把「解析」「连接」「关闭」「业务」拆到对应模块，便于 Step7 加定时器、Step8 改 write 路径。

---

## 第十部分：本篇小结与下一篇预告

### 你现在已经会了什么

- [x] HTTP 三态状态机（请求行 → Header → Body）
- [x] `read_buf_` 处理 TCP 半包 / 粘包
- [x] `append_read` + `process_read` 分工
- [x] POST `/echo` 回显 Body
- [x] GET `/echo` 返回 `www/echo.html` 表单
- [x] `connections` 映射表管理连接生命周期
- [x] `do_request` 作为统一业务入口（Step10 登录注册在此基础上扩展）

### 系列进度

```text
Step5 线程池
    → Step6 状态机 + POST  ← 你在这里
    → Step7 定时器（空闲连接超时）
    → Step8 主线程 EPOLLOUT 分次写
    → Step9 日志 + eventfd
    → Step10 MySQL 注册登录
```

### 建议实验

1. 用 `curl -v` 发 POST，对照 `read_buf_` 打印（临时加日志）理解分包。
2. 把 `max_body_size` 改成 `10`，POST 大于 10 字节的 body，观察 Error 路径。
3. 读 [step11.cpp](../../step11.cpp) 里的 `process_read` / `handle_read`，对比后续 Step7～11 的演进。

---

> **下一步：** [07-Step7-定时器](./07-Step7-定时器.md) —— 为空闲连接加超时关闭，防止 `connections` 泄漏。
