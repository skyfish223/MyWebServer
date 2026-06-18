# HTTP 状态机 + POST —— 零基础学习指南（第六篇）

> **写给谁看：** 已完成 [第五篇](./05-线程池指南.md)（[Step5.cpp](../Step5.cpp)）的学习者——会用 epoll + 线程池处理 GET 静态文件。  
> **本篇目标：** 不再只解析**请求行**；用 **HTTP 状态机** 按阶段读完「请求行 → Header → Body」，并支持 **POST**（先实现 `/echo` 回显 body，不接数据库）。  
> **测试方式：** 浏览器打开 `www/echo.html` 表单提交（推荐）+ `curl` 命令（快速验收），**不是**只能 curl。  
> **不做的事：** 不做定时器、不做分次写（EPOLLOUT）、不做 MySQL —— 这些留到 Step7～Step10。  
> **前置文档：** [01](./01-最小可行Web服务器指南.md) · [02](./02-解析HTTP请求指南.md) · [03](./03-返回静态文件指南.md) · [04](./04-epoll高并发指南.md) · [05](./05-线程池指南.md) · [Step5 之后路线图](./Step5之后完善路线图.md)

---

## 目录

1. [第一部分：先搞懂「我们在升级什么」](#第一部分先搞懂我们在升级什么)
2. [第二部分：需要哪些新「工具」，为什么要用](#第二部分需要哪些新工具为什么要用)
3. [第三部分：完整代码 + 逐块讲解](#第三部分完整代码--逐块讲解)
4. [编译、运行与测试](#编译运行与测试)
5. [常见问题排查](#常见问题排查)
6. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

---

## 第一部分：先搞懂「我们在升级什么」

### 1.1 Step5 的瓶颈在哪？

Step5 里，主线程 `read` 一次就把整个 buffer 当字符串，然后 `parseRequestLine` 解析**第一行**：

```text
read 一次
  → parseRequestLine（只认 GET /path HTTP/1.1）
  → buildResponseFromRequest（静态文件）
  → write + close
```

| 问题 | 说明 |
|------|------|
| **只认请求行** | Header（如 `Content-Length`）和 Body 被忽略 |
| **不支持 POST** | 登录表单的数据在 Body 里，Step5 永远拿不到 |
| **假定一次 read 收齐** | TCP 可能分包：第一次只收到半行、半个 Header |
| **read 完就 removeFd** | 数据没收齐时连接被关掉，无法「等下次再读」 |

对小 GET 页面、本机 curl 往往「碰巧能跑」；一旦浏览器发 POST，或网络分包，就会出 bug。

---

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
浏览器 POST 登录时，用户名密码在 **Body**；Body 多长由 Header 里的 **`Content-Length`** 声明。不解析 Header，就无法正确读 Body。

---

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

---

### 1.4 什么叫「HTTP 状态机」？

「状态机」不是新协议，而是**解析时的步骤流程**：用变量记录「当前解析到哪一层」，每收到一点数据就根据当前层决定下一步。

#### 三个主状态（对齐 TinyWebServer）

| 状态 | 枚举名（本篇） | 在干什么 |
|------|---------------|---------|
| 解析请求行 | `ParseState::RequestLine` | 读 `GET / HTTP/1.1` |
| 解析 Header | `ParseState::Header` | 逐行读 `Host:`、`Content-Length:` … |
| 收 Body | `ParseState::Content` | 按 `Content-Length` 收 N 字节 |

主循环在 `process_read()` 里 `switch (state_)`，根据状态调用不同的 `parse_*` 函数。

#### 主状态机 vs 从状态机

| 层次 | 做什么 | 本篇实现 |
|------|--------|---------|
| **从状态机** | 从字节流里**切出一行**（`\r\n` 是否完整） | `get_line(read_buf_, line)` |
| **主状态机** | 这一行属于请求行 / 头 / 体 | `process_read()` + `switch(state_)` |

学习阶段可以**简化从状态机**：在 `read_buf_` 里用 `find("\r\n")` 取行；行不完整就返回「再等数据」。

#### 和 Step5「一次 read 整包」的区别

```text
Step5：
  read 一次 → 整个 buffer → parseRequestLine → 完事

Step6（状态机）：
  每次 read 追加到 read_buf_
  调用 process_read()：
    - 能解析几行就解析几行
    - 数据不够 → 返回 Incomplete，fd 继续留在 epoll，下次再读
    - 三段都齐 → 进入 do_request（静态文件 / POST echo）
```

#### 三态流程图

```text
        ┌──────────────────────┐
        │ REQUESTLINE          │
        │ 解析 method/path/ver │
        └──────────┬───────────┘
                   │ 成功
                   ▼
        ┌──────────────────────┐
        │ HEADER               │
        │ 逐行 parse_headers   │
        └──────────┬───────────┘
                   │
         ┌─────────┴─────────┐
         │ 空行              │
    GET  │                   │ POST 且 Content-Length > 0
         ▼                   ▼
   可以 do_request      ┌──────────────────────┐
                         │ CONTENT              │
                         │ 收满 N 字节 body     │
                         └──────────┬───────────┘
                                    ▼
                              可以 do_request
```

**POST 不是单独一个 `if (method == "POST")`**，而是状态机跑通后的自然结果：必须经过 **HEADER → CONTENT** 才能拿到 body。

---

### 1.5 本篇架构（在 Step5 上改什么）

```text
┌─────────────────────────────────────────────────────────────┐
│  main 线程（I/O Reactor）                                    │
│                                                              │
│  epoll_wait                                                  │
│    listen_fd 就绪 → accept → 新建 HttpConnection → addFd     │
│    client_fd 就绪 → handle_read(conn)                        │
│                      ├─ append_read（追加到 read_buf_）       │
│                      ├─ process_read（状态机）               │
│                      │     Incomplete → 保持 on epoll        │
│                      │     Complete   → removeFd + submit    │
│                      │     Error      → close                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
                     ThreadPool 工作线程
                       do_request(req) → write → close
```

**口诀：主线程管「读 + 拼包 + 状态机」；工人管「做菜、上菜、收盘」。**

与 Step5 的关键区别：

| 项目 | Step5 | Step6 |
|------|-------|-------|
| 解析 | 只解析请求行 | 三态状态机 |
| read | 一次 read 就 removeFd | 没收齐就**继续等** EPOLLIN |
| 连接状态 | lambda + 裸 fd | **`HttpConnection` 对象** |
| 业务 | 只 GET 静态文件 | GET 静态文件 + **POST /echo 回显** |
| `/echo` 路径 | — | **GET** 返回测试页；**POST** 回显 body（见 1.6） |

---

### 1.6 同一路径 `/echo`：GET 和 POST 是两回事

很多人第一次在浏览器输入 `http://127.0.0.1:8080/echo`，若没有任何静态页，会看到 **404 - 文件不存在**——这不是 bug，而是 **Method 不同，服务器走不同逻辑**：

| 你怎么访问 | HTTP 方法 | 服务器做什么 |
|-----------|----------|-------------|
| 地址栏输入 `/echo` | **GET** | 读磁盘 `www/echo.html`，返回测试表单页 |
| 点击表单「提交」 | **POST** | 状态机收齐 body → `buildPostEchoResponse` 回显 |
| `curl -X POST .../echo -d "a=b"` | **POST** | 同上 |

```text
浏览器地址栏 ──→ 只能 GET ──→ pathToFile("/echo") ──→ www/echo.html
表单 / curl   ──→ POST     ──→ do_request POST 分支 ──→ 回显 req.body
```

**生活类比：** `/echo` 像一家店——**推门看菜单（GET 要网页）** 和 **填单交货（POST 交数据）** 是两种业务，不能指望「只看菜单」就帮你把货 echo 回来。

Step10 登录注册也是这个模式：`GET /log.html` 显示表单，`POST /login` 提交用户名密码。

---

### 1.7 对照 TinyWebServer 怎么读

| 本篇概念 | TinyWebServer 文件 |
|---------|-------------------|
| `ParseState` | `http/http_conn.h` 里 `CHECK_STATE_*` |
| `process_read` | `http/http_conn.cpp` → `process_read()` |
| `parse_request_line` | 同名函数 |
| `parse_headers` | 同名函数 |
| `parse_content` | 同名函数 |

不必和 Tiny 一模一样；**顺序和能力对齐**即可。

---

## 第二部分：需要哪些新「工具」，为什么要用

### 2.1 新增头文件

| 头文件 | 提供什么 | 为什么需要 |
|--------|---------|-----------|
| `<map>` | `std::map` | 存 Header 键值对（如 `content-length` → `11`） |
| `<unordered_map>` | `std::unordered_map` | `fd → HttpConnection` 连接表 |
| `<algorithm>` | `transform` 等 | Header 名转小写（可选） |
| `<cctype>` | `tolower` | 同上 |

Step5 已有头文件**全部保留**。

### 2.2 新增常量

```cpp
const size_t MAX_BODY_SIZE = 1024 * 1024;  // POST body 上限 1MB，防恶意大包
```

### 2.3 新增：`ParseState` 与扩展后的 `HttpRequest`

```cpp
enum class ParseState {
    RequestLine,
    Header,
    Content
};

struct HttpRequest {
    string method;
    string path;
    string version;
    map<string, string> headers;   // key 统一小写
    size_t content_length = 0;
    string body;
};
```

| 字段 | 含义 |
|------|------|
| `headers` | 如 `host`、`content-length`、`content-type` |
| `content_length` | 从 Header 解析出的 Body 长度 |
| `body` | POST 正文（如 `username=abc&passwd=123`） |

### 2.4 新增：`HttpConnection` 类

每个 `client_fd` 对应一个连接对象，把「读缓冲 + 解析状态 + 已解析请求」绑在一起：

```cpp
class HttpConnection {
public:
    explicit HttpConnection(int fd) : fd_(fd) {}

    int fd_;
    ParseState state_ = ParseState::RequestLine;
    string read_buf_;              // 尚未解析完的接收数据
    HttpRequest request_;
};
```

Step7 定时器、Step8 分次写都会在这个类上继续加字段（`last_active_`、`write_buf_` 等），**越早抽类越省事**。

### 2.5 新增静态文件：`www/echo.html`

光有 POST 接口还不够——浏览器地址栏只能 GET。本篇在 `www/` 增加 **`echo.html`**，并在 `pathToFile` 里把 `/echo` 映射过去（与 `/` → `index.html` 同理）。

表单核心（完整文件见 [www/echo.html](../www/echo.html)）：

```html
<form action="/echo" method="post">
  <input name="msg" type="text" value="hello=world">
  <textarea name="note">Step6 状态机 POST 测试</textarea>
  <button type="submit">POST 到 /echo</button>
</form>
```

| 文件 | 作用 |
|------|------|
| `www/echo.html` | GET 时展示的 POST 测试页 |
| `www/index.html` | 首页增加链接 `<a href="/echo">POST /echo 测试页</a>` |
| `pathToFile` | `/echo`、`/echo.html` → `www/echo.html` |

### 2.6 新增 / 改造函数一览

| 函数 | 职责 |
|------|------|
| **`append_read(conn)`** | `read` 追加到 `read_buf_` |
| **`get_line(buf, line)`** | 从缓冲取一行；不够完整行返回 false |
| **`parse_request_line(line, req)`** | 解析请求行；成功则调用方切到 Header 态 |
| **`parse_header_line(line, req)`** | 解析一行 Header；识别 `Content-Length` |
| **`process_read(conn)`** | 主状态机；返回 Complete / Incomplete / Error |
| **`do_request(req)`** | GET 走静态文件；POST `/echo` 回显 body |
| **`handle_read(epfd, conn, pool)`** | 替代 Step5 的 `dispatchClient` |
| ~~`parseRequestLine(raw, req)`~~ | 改为按**行**解析，不再从整包 raw 里 find 第一行 |
| ~~`processRawRequest(raw)`~~ | 改为 `do_request(req)`，入参是已解析好的 `HttpRequest` |
| **`pathToFile`（小改）** | 增加 `/echo` → `www/echo.html` 映射 |

### 2.7 Header 至少要认哪些

| Header | 本篇用途 |
|--------|---------|
| `Host` | HTTP/1.1 规范要求；先存下来，业务暂不用 |
| `Content-Length` | **必须**；决定 Body 要读多少字节 |
| `Content-Type` | POST 表单常见 `application/x-www-form-urlencoded`；echo 可先打印 |
| `Connection: keep-alive` | Step12 再用；本篇仍响应 `Connection: close` |

解析方式：找第一个 `:`，左边是 key（转小写），右边 trim 后是 value。

### 2.8 `process_read` 返回值设计

```cpp
enum class ReadResult {
    Incomplete,   // 数据不够，fd 继续留在 epoll
    Complete,     // 请求完整，可以 do_request
    Error         // 格式错误，关连接
};
```

这样主循环逻辑非常清晰：

```text
append_read
process_read
  Incomplete → return（等下次 EPOLLIN）
  Error      → removeFd + close
  Complete   → removeFd + pool.submit(do_request + write + close)
```

---

## 第三部分：完整代码 + 逐块讲解

### 3.1 完整代码

保存为 **`Step1to6/step6.cpp`**（Step1～6 单文件源码均在 [`Step1to6/`](../Step1to6/) 目录）：

```cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

const string WEB_ROOT = "www";
const int MAX_EVENTS = 64;
const int PORT = 8080;
const size_t THREAD_POOL_SIZE = 4;
const size_t MAX_BODY_SIZE = 1024 * 1024;

// ========== 第六篇：HTTP 状态机 ==========
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
    string method;
    string path;
    string version;
    map<string, string> headers;
    size_t content_length = 0;
    string body;
};

class HttpConnection {
public:
    explicit HttpConnection(int fd) : fd_(fd) {}

    int fd_;
    ParseState state_ = ParseState::RequestLine;
    string read_buf_;
    HttpRequest request_;
};

static string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static string toLower(string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
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
    if (!(iss >> req.method >> req.path >> req.version)) return false;
    return true;
}

void parse_header_line(const string& line, HttpRequest& req) {
    size_t colon = line.find(':');
    if (colon == string::npos) return;
    string key = toLower(trim(line.substr(0, colon)));
    string val = trim(line.substr(colon + 1));
    req.headers[key] = val;
    if (key == "content-length") {
        req.content_length = static_cast<size_t>(stoul(val));
    }
}

ReadResult process_read(HttpConnection& conn) {
    string line;
    while (true) {
        if (conn.state_ == ParseState::Content) {
            if (conn.request_.content_length > MAX_BODY_SIZE)
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
                if (conn.request_.method == "POST" && conn.request_.content_length > 0) {
                    conn.state_ = ParseState::Content;
                } else {
                    return ReadResult::Complete;
                }
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
    char buffer[4096];
    ssize_t n = read(conn.fd_, buffer, sizeof(buffer));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    if (n == 0) return false;
    conn.read_buf_.append(buffer, static_cast<size_t>(n));
    return true;
}

// ========== 第二～五篇：HTTP 响应 + 静态文件（扩展 do_request）==========
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
    if (urlPath == "/echo" || urlPath == "/echo.html")
        return WEB_ROOT + "/echo.html";
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

string buildGetResponse(const HttpRequest& req) {
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

string buildPostEchoResponse(const HttpRequest& req) {
    string msg = "收到 POST body:\n" + req.body;
    return buildHttpResponse(200, "OK",
        "text/plain; charset=utf-8", msg);
}

string do_request(const HttpRequest& req) {
    if (req.method == "GET") {
        return buildGetResponse(req);
    }
    if (req.method == "POST") {
        if (req.path == "/echo")
            return buildPostEchoResponse(req);
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "未知 POST 路径: " + req.path);
    }
    return buildHttpResponse(405, "Method Not Allowed",
        "text/plain; charset=utf-8", "暂只支持 GET 和 POST /echo");
}

// ========== 第五篇：线程池（与 Step5 相同）==========
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount) : stop_(false) {
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            lock_guard<mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    void submit(function<void()> task) {
        {
            lock_guard<mutex> lock(mtx_);
            tasks_.push(move(task));
        }
        cv_.notify_one();
    }

private:
    void workerLoop() {
        while (true) {
            function<void()> task;
            {
                unique_lock<mutex> lock(mtx_);
                cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    queue<function<void()>> tasks_;
    mutex mtx_;
    condition_variable cv_;
    vector<thread> workers_;
    bool stop_;
};

// ========== 第四篇：epoll 工具 ==========
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void addFd(int epfd, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void removeFd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

void closeConnection(int epfd, int fd,
                     unordered_map<int, HttpConnection>& connections) {
    removeFd(epfd, fd);
    close(fd);
    connections.erase(fd);
}

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& connections) {
    if (!append_read(conn)) {
        cout << "[main] 读失败或连接关闭 fd=" << conn.fd_ << "\n";
        closeConnection(epfd, conn.fd_, connections);
        return;
    }

    ReadResult result = process_read(conn);

    if (result == ReadResult::Incomplete) {
        return;
    }

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

int main() {
    ThreadPool pool(THREAD_POOL_SIZE);
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

    cout << "Step6 状态机服务器已启动：http://127.0.0.1:" << PORT << "\n";
    cout << "工作线程数: " << THREAD_POOL_SIZE << "\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";
    cout << "浏览器测试: http://127.0.0.1:" << PORT << "/echo\n";
    cout << "curl 测试: curl -X POST http://127.0.0.1:" << PORT << "/echo -d 'a=b'\n";

    vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
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
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
```

---

### 3.2 逐块讲解：`get_line`（从状态机）

```cpp
bool get_line(string& buf, string& line) {
    size_t pos = buf.find("\r\n");
    if (pos == string::npos) return false;
    line = buf.substr(0, pos);
    buf.erase(0, pos + 2);
    return true;
}
```

| 步骤 | 含义 |
|------|------|
| `find("\r\n")` | HTTP 行以 `\r\n` 结尾；找不到说明**行还没收齐** |
| `substr(0, pos)` | 取出不含 `\r\n` 的一行 |
| `erase(0, pos + 2)` | 从缓冲里删掉已消费的这一行 |

**为什么必须认 `\r\n`？**  
若只认 `\n`，可能把 `\r` 留在字符串里，导致 Header 名匹配失败；空行也可能认不出来。

---

### 3.3 逐块讲解：`parse_header_line`

```cpp
void parse_header_line(const string& line, HttpRequest& req) {
    size_t colon = line.find(':');
    if (colon == string::npos) return;
    string key = toLower(trim(line.substr(0, colon)));
    string val = trim(line.substr(colon + 1));
    req.headers[key] = val;
    if (key == "content-length") {
        req.content_length = static_cast<size_t>(stoul(val));
    }
}
```

| 要点 | 说明 |
|------|------|
| key 转小写 | `Content-Length` 和 `content-length` 都能匹配 |
| 单独存 `content_length` | Content 态直接比较字节数，不用每次查 map |
| `stoul` | 把字符串 `"11"` 转成数字 11 |

---

### 3.4 逐块讲解：`process_read`（主状态机）

这是本篇**最核心**的函数。读的时候建议对照下面流程：

```text
loop:
  若当前是 Content 态：
      read_buf_ 够 content_length 字节吗？
        够 → 切出 body，返回 Complete
        不够 → 返回 Incomplete

  从 read_buf_ 取一行：
      取不出 → 返回 Incomplete

  若当前是 RequestLine 态：
      解析行 → 失败 Error；成功 → 切 Header 态

  若当前是 Header 态：
      空行？
        GET 或 POST 无 body → Complete
        POST 且 content_length > 0 → 切 Content 态（可能 read_buf_ 里已有部分 body）
      非空行 → parse_header_line
```

**Content 态为什么要单独判断，而不是再用 `get_line`？**  
Body 是**二进制字节流**，不一定按行分割；必须按 `Content-Length` 数**固定 N 个字节**。

**POST 且 `Content-Length: 0` 怎么办？**  
Header 空行后直接 `Complete`，不进入 Content 态。

---

### 3.5 逐块讲解：`handle_read`（替代 `dispatchClient`）

Step5 的 `dispatchClient`：

```text
read 一次 → removeFd → submit（假定请求已完整）
```

Step6 的 `handle_read`：

```text
append_read
process_read
  Incomplete → 什么都不做，fd 仍在 epoll 上
  Error      → closeConnection
  Complete   → removeFd + 从 connections 删除 + submit(do_request)
```

| 对比 | Step5 | Step6 |
|------|-------|-------|
| 何时 removeFd | read 后立刻 | **请求完整后**才 remove |
| 半包 | 会解析失败或丢数据 | 下次 EPOLLIN 继续 append |

**lambda 里捕获 `req`（按值拷贝）**：主线程继续跑 epoll，不能把栈上引用交给工作线程。

---

### 3.6 逐块讲解：`do_request`

```cpp
string do_request(const HttpRequest& req) {
    if (req.method == "GET")  return buildGetResponse(req);
    if (req.method == "POST") {
        if (req.path == "/echo") return buildPostEchoResponse(req);
        ...
    }
    ...
}
```

| 路由 | 行为 |
|------|------|
| `GET /`、`GET /about.html` | 与 Step3～5 相同，读 `www/` |
| **`GET /echo`、`GET /echo.html`** | 返回 `www/echo.html` 测试表单页 |
| **`POST /echo`** | 把收到的 body 原样拼进响应文本（**不是**读磁盘） |
| 其它 POST | 404 |
| PUT/DELETE 等 | 405 |

**为什么第一个 POST 业务选 `/echo`？**  
Step10 要做登录注册，必须先证明「Body 能完整收到」；echo 是最小、最直观的验收接口，**不要一上来接 MySQL**。

**为什么还要有 `echo.html`？**  
地址栏只能 GET；测试页 + 表单 = Step10 登录页的原型（`log.html` + `POST /login`）。

---

### 3.7 逐块讲解：`pathToFile` 与 `www/echo.html`

```cpp
string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html")
        return WEB_ROOT + "/index.html";
    if (urlPath == "/echo" || urlPath == "/echo.html")
        return WEB_ROOT + "/echo.html";
    return WEB_ROOT + urlPath;
}
```

若没有 `/echo` 这一行，浏览器 GET `/echo` 会去找 `www/echo`（无扩展名文件），返回 **404**。  
加上映射后，GET 返回 HTML 表单；用户点提交时浏览器发 **POST /echo**，才进入 `buildPostEchoResponse`。

---

### 3.8 连接表 `connections`

```cpp
unordered_map<int, HttpConnection> connections;
```

| 时机 | 操作 |
|------|------|
| `accept` 成功 | `connections.emplace(client_fd, HttpConnection(client_fd))` |
| 请求完整 / 出错 / 读失败 | `connections.erase(fd)` |

每个 fd 的 `read_buf_`、`state_`、`request_` 都存在对应对象里，不会和别的连接搞混。

---

### 3.9 与 Step5 对照表

| 项目 | Step5 | Step6 |
|------|-------|-------|
| 解析范围 | 仅请求行 | 请求行 + Header + Body |
| HTTP 方法 | 仅 GET | GET + POST `/echo` |
| 连接模型 | 裸 fd + lambda | **`HttpConnection` + 连接表** |
| read 策略 | 一次 read | **循环 append 直到完整** |
| removeFd 时机 | read 后立刻 | **process_read 返回 Complete 后** |
| 线程池分工 | 不变 | 不变（仍做 do_request + write + close） |
| 静态资源 | `www/index.html` 等 | 另增 **`www/echo.html`** + `pathToFile` 映射 |

---

## 编译、运行与测试

### 4.1 编译

在**项目根目录**执行：

```bash
g++ -std=c++17 -Wall -pthread -o server Step1to6/step6.cpp
```

> 必须加 **`-pthread`**。源码见 [Step1to6/step6.cpp](../Step1to6/step6.cpp)。

### 4.2 运行

在**项目根目录**（有 `www/` 和 `www/echo.html`；源码在 `Step1to6/`）：

```bash
./server
```

应看到：

```text
Step6 状态机服务器已启动：http://127.0.0.1:8080
工作线程数: 4
网站根目录: www/
浏览器测试: http://127.0.0.1:8080/echo
curl 测试: curl -X POST http://127.0.0.1:8080/echo -d 'a=b'
```

### 4.3 GET 回归测试（确保没改坏 Step3～5）

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/about.html
curl -I http://127.0.0.1:8080/css/style.css
curl http://127.0.0.1:8080/echo
```

最后一条应返回 **HTML**（echo 测试页），而不是 `404 - 文件不存在`。

浏览器打开 `http://127.0.0.1:8080/`，首页应有 **「POST /echo 测试页」** 链接；页面和样式应与 Step5 一致。

### 4.4 浏览器测试 POST /echo（推荐）

1. 打开 `http://127.0.0.1:8080/echo`（或从首页点链接）  
2. 应看到带输入框的 **POST /echo 测试页**（这是 **GET**，读的是 `echo.html`）  
3. 点击 **「POST 到 /echo」**  
4. 浏览器会显示纯文本页，内容类似：

```text
收到 POST body:
msg=hello%3Dworld&note=Step6+状态机+POST+测试
```

（浏览器会对表单字段做 URL 编码，`%3D` 即 `=`，这是正常现象。）

终端 worker 日志应出现 `POST /echo body_len=...`。

### 4.5 curl 测试 POST /echo（快速验收）

```bash
curl -X POST http://127.0.0.1:8080/echo -d "hello=world"
```

期望响应 body 中包含：

```text
收到 POST body:
hello=world
```

再试表单格式：

```bash
curl -X POST http://127.0.0.1:8080/echo \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "username=abc&passwd=123456"
```

终端 worker 日志应出现 `POST /echo body_len=27`（或类似）。

> **curl 和浏览器的区别：** curl 适合脚本化、一行命令验收；浏览器表单更接近 Step10 登录注册的真实用法。两种都应通过。

### 4.6 错误路径测试

```bash
curl -X POST http://127.0.0.1:8080/notfound -d "x=1"
# 期望 404

curl -X PUT http://127.0.0.1:8080/echo -d "x=1"
# 期望 405
```

### 4.7 进阶自测：分包（可选）

用 Python 模拟「先发 Header、sleep、再发 Body」：

```python
import socket, time
s = socket.create_connection(("127.0.0.1", 8080))
s.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n")
time.sleep(1)
s.sendall(b"hello")
print(s.recv(4096).decode())
s.close()
```

若仍返回 200 且 body 含 `hello`，说明状态机在**多次 read** 下工作正常。

### 4.8 成功 checklist

| 检查项 | 预期 |
|--------|------|
| GET 首页 + CSS | 正常显示 |
| **GET /echo** | 显示 echo 测试表单页（HTML） |
| **浏览器提交表单** | 200，纯文本回显 POST body |
| **curl POST /echo** | 200，body 回显提交内容 |
| 终端 | `[main]` 读包；`[worker]` 处理业务 |
| 未知 POST 路径 | 404 |
| 非 GET/POST | 405 |

---

## 常见问题排查

### Q0：浏览器打开 `/echo` 显示「404 - 文件不存在」

| 可能原因 | 处理 |
|---------|------|
| 没有 `www/echo.html` | 从仓库复制或按 2.5 节创建 |
| `pathToFile` 未映射 `/echo` | 加上 `if (urlPath == "/echo" ...) return WEB_ROOT + "/echo.html";` |
| 误以为地址栏会触发 POST echo | 地址栏是 **GET**；要测回显请**提交表单**或用 curl |

### Q1：POST 返回 400 或 body 为空

| 可能原因 | 检查 |
|---------|------|
| 没解析 `Content-Length` | curl 是否带 `-d`（会自动加 Length） |
| Header 空行认错了 | `get_line` 是否用 `\r\n` |
| 还没收齐 body 就 `do_request` | `process_read` 在 Content 态是否判断 `read_buf_.size() >= content_length` |

### Q2：GET 正常，POST 一直卡住

Body 还没收齐，`process_read` 返回 `Incomplete`，连接会一直等——这是**正确行为**。  
若永远不收齐：检查客户端是否真的发了 `Content-Length` 个字节；或用 curl 对比。

### Q3：编译 `stoul` 抛异常

Header 里 `Content-Length` 不是数字；恶意请求。本篇可 catch 后返回 Error 关连接。

### Q4：和 Step5 比，连接数变多、fd 占用更久

Step6 在「请求没收齐」前不会 removeFd，这是为分包和 POST 必须的。Step7 会加**定时器**踢掉慢连接。

### Q5：能否在 `process_read` 里直接 `do_request`？

可以，但会阻塞 epoll 主线程读盘。本篇保持 Step5 分工：**主线程解析，工作线程业务**。

### Q6：Header 很多行会有性能问题吗？

学习项目完全够用。生产环境会用固定大小读缓冲 + 指针索引（Tiny 的 `m_read_idx` / `m_checked_idx`），不必第一步就照搬。

### Q7：是不是只能 curl 测 POST？

**不是。** 推荐顺序：

1. 浏览器打开 `/echo` → 提交表单（最贴近 Step10 登录）  
2. `curl -X POST ...`（最快脚本验收）  
3. Python 分包脚本（测状态机，见 4.7 节）

### Q8：Windows 能编译吗？

网络部分建议 **WSL2 / Linux**；`epoll`、`read`/`write` 与 Windows 原生 API 不同。

---

## 本篇小结与下一篇预告

### 你现在已经会了什么

- [x] 理解 **HTTP 三态状态机**：请求行 → Header → Body  
- [x] 用 **`HttpConnection`** 保存每个连接的读缓冲和解析状态  
- [x] 实现 **`get_line` + `process_read`**，支持 TCP 分包  
- [x] 解析 **`Content-Length`**，完整接收 POST body  
- [x] 实现 **`POST /echo`** 作为登录前的验收接口  
- [x] 提供 **`www/echo.html`**：GET 看页、表单 POST 测 echo  
- [x] 主循环：**Incomplete 继续等，Complete 再 submit 线程池**  

### Step5 → Step6 → Step7 路线图

```text
Step5  线程池 + epoll + GET 静态文件
    ▼
Step6  HTTP 状态机 + Header + POST /echo     ← 本篇
    ▼
Step7  定时器（踢掉非活跃连接）
    ▼
Step8  分次写（EPOLLOUT）+ write_buf_
```

完整路线见：[Step5 之后完善路线图](./Step5之后完善路线图.md)

### 建议你自己做的三个小实验

1. 在 `parse_header_line` 里打印每个 Header，用浏览器访问 `/echo` 并提交表单，对比 GET 与 POST 的 Header 差异。  
2. 把 `MAX_BODY_SIZE` 改成 `10`，在 echo 页提交长文本，看是否关连接。  
3. 在 `process_read` 的 Content 态加日志，用 Python 分包脚本，观察**两次 read** 才 Complete。

### 下一篇：Step7 定时器

Step6 连接在「半包」时会一直占着 fd。若客户端连上后不发数据，或极慢地发，服务器资源会被占满。  
Step7 将为每个连接记录**最后活跃时间**，超时则 `close` + 从 epoll 删除。详见 [07-定时器指南](./07-定时器指南.md)。

### 易错点速查表

| 错误 | 后果 |
|------|------|
| 只判断 `method == POST` 不读 body | 永远拿不到表单数据 |
| 用 `strlen` 算 body 长度 | 二进制或 `\0` 会错；必须用 `Content-Length` |
| Header 没读完就 `do_request` | body 粘在 Header 后面被当垃圾 |
| 空行只认 `\n` 不认 `\r\n` | 进不了 Content 态 |
| `Content-Length` 不设上限 | 恶意大包可能占满内存 |
| 地址栏访问 `/echo` 期望看到 echo 回显 | 地址栏是 GET，只会打开测试页；回显需 POST |

---

> **恭喜：** 走完本篇，你的服务器已经能**完整解析 HTTP 请求**，并正确处理 POST。浏览器表单 + curl 都能验收；这也是 Step10「MySQL 登录注册」的必要前置——用户名和密码，就在你刚刚收齐的 `req.body` 里。
