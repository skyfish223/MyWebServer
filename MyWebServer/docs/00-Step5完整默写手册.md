# MyWebServer 总览 + Step5 完整默写手册

> **本文两个用途：**  
> ① 理解五篇代码是怎么一层层叠上去的；  
> ② **只读这一份 MD**，就能从零完整写出 [Step5.cpp](../Step5.cpp)。  
> 建议：先通读第一～三部分理解流程，默写时按第四部分「书写顺序」+ 第五部分「完整代码」对照。  
> **Step5 之后怎么继续完善（Step6～12 路线图）见：** [06-Step5之后完善路线图](./06-Step5之后完善路线图.md)

---

## 目录

**理解篇**

1. [五篇演进：每层解决什么问题](#一五篇演进每层解决什么问题)
2. [Step5 总架构一张图](#二step5-总架构一张图)
3. [一次请求的完整生命周期](#三一次请求的完整生命周期)
4. [函数清单与依赖关系](#四函数清单与依赖关系)
5. [头文件与常量：为什么需要](#五头文件与常量为什么需要)

**默写篇**

6. [默写顺序（推荐 12 步）](#六默写顺序推荐-12-步)
7. [分块模板：每块要写什么](#七分块模板每块要写什么)
8. [main 函数逐步清单](#八main-函数逐步清单)
9. [易错点检查表](#九易错点检查表)
10. [完整 Step5.cpp 源码（对照用）](#十完整-step5cpp-源码对照用)

---

# 理解篇

---

## 一、五篇演进：每层解决什么问题

```text
Step1  socket 开店
         │  固定返回 Hello World
         ▼
Step2  解析 HTTP 请求行（GET /path）
         │  不同 URL 不同内容（写死在代码里）
         ▼
Step3  静态文件 www/
         │  页面在磁盘，不再写死在 C++ 字符串
         ▼
Step4  epoll
         │  单进程监视多连接，不再 accept 阻塞串行
         ▼
Step5  线程池
           主线程：epoll + read + 投递任务
           工作线程：解析 + 读文件 + write + close
```

| 步骤 | 新增能力 | 核心 API / 类 |
|------|---------|--------------|
| **1** | 能响应浏览器 | `socket bind listen accept read write close` |
| **2** | 懂请求、能路由 | `find substr istringstream` → `HttpRequest` |
| **3** | 读磁盘 HTML/CSS | `ifstream` → `pathToFile readFile getContentType` |
| **4** | 多连接 | `epoll_create1 epoll_ctl epoll_wait fcntl O_NONBLOCK` |
| **5** | 多核并行业务 | `ThreadPool` + `mutex condition_variable` |

**Step5 = Step1～4 全部保留 + 线程池分工。**

---

## 二、Step5 总架构一张图

```text
┌─────────────────────────────────────────────────────────────┐
│  main 线程（I/O Reactor）                                    │
│                                                              │
│  socket → bind → listen → epoll_create1 → addFd(listen)     │
│       │                                                      │
│       └── while(true) {                                      │
│             epoll_wait                                       │
│               ├─ listen_fd 就绪 → while accept → addFd(client)│
│               └─ client_fd 就绪 → dispatchClient             │
│                     ├─ read 请求                             │
│                     ├─ removeFd                              │
│                     └─ pool.submit(lambda)  ───────┐         │
│           }                                        │         │
└────────────────────────────────────────────────────│─────────┘
                                                     │
                     ┌───────────────────────────────▼─────────┐
                     │  ThreadPool 任务队列（mutex 保护）        │
                     └───────────────────────────────┬─────────┘
                                                     │
              ┌──────────────┬──────────────┬────────▼────────┐
              │  worker 1    │  worker 2    │  worker 3/4 ... │
              │ processRaw   │ processRaw   │                 │
              │ write close  │ write close  │                 │
              └──────────────┴──────────────┴─────────────────┘
```

**口诀：主线程管「等事件、读请求」；工人管「做菜、上菜、收盘」。**

---

## 三、一次请求的完整生命周期

以浏览器访问 `http://127.0.0.1:8080/` 为例：

```text
① 浏览器发：GET / HTTP/1.1\r\nHost:...\r\n\r\n

② epoll_wait：listen_fd 或 client_fd 就绪

③ 若是新连接：
   accept → setNonBlocking → addFd(client_fd)
   （等下一次 EPOLLIN 再 read）

④ client_fd 可读 → dispatchClient：
   read → 得到 rawRequest
   removeFd（从 epoll 摘掉）
   pool.submit(lambda)

⑤ 工作线程 lambda：
   processRawRequest(rawRequest)
     → parseRequestLine → method=/ path=/
     → buildResponseFromRequest
         → isPathSafe → pathToFile → www/index.html
         → readFile → getContentType
         → buildHttpResponse(200, ...)
   write → close

⑥ 浏览器收到 HTML；可能再请求 /css/style.css，重复 ②～⑤
```

---

## 四、函数清单与依赖关系

按 **Step5.cpp 文件从上到下** 的顺序：

| 序号 | 函数 / 类 | 来自哪篇 | 干什么 |
|------|----------|---------|--------|
| 1 | `HttpRequest` | 2 | 存 method / path / version |
| 2 | `parseRequestLine` | 2 | 原始报文 → HttpRequest |
| 3 | `buildHttpResponse` | 2 | body → 完整 HTTP 响应字符串 |
| 4 | `pathToFile` | 3 | URL → www/... |
| 5 | `isPathSafe` | 3 | 禁止 `..` |
| 6 | `getContentType` | 3 | 后缀 → MIME |
| 7 | `readFile` | 3 | 磁盘 → string |
| 8 | `buildResponseFromRequest` | 3 | req → 响应（GET/403/404/200） |
| 9 | `processRawRequest` | 5 | raw → 解析 + 8 |
| 10 | `ThreadPool` | 5 | 任务队列 + 工作线程 |
| 11 | `setNonBlocking` | 4 | fcntl 非阻塞 |
| 12 | `addFd` / `removeFd` | 4 | epoll_ctl ADD/DEL |
| 13 | `dispatchClient` | 5 | read + submit |
| 14 | `main` | 1+4+5 | 启动 + epoll 循环 |

**依赖链（业务）：**

```text
processRawRequest
  → parseRequestLine
  → buildResponseFromRequest
      → buildHttpResponse
      → isPathSafe → pathToFile → readFile → getContentType
```

---

## 五、头文件与常量：为什么需要

### 头文件（按 include 顺序记）

| 头文件 | 用于 |
|--------|------|
| `<iostream>` | cout cerr |
| `<fstream>` | readFile |
| `<sstream>` | istringstream 解析请求行 |
| `<string>` | string |
| `<vector>` | vector\<epoll_event\> workers_ |
| `<queue>` | tasks_ 队列 |
| `<thread>` | std::thread |
| `<mutex>` | mutex lock_guard |
| `<condition_variable>` | cv_ |
| `<functional>` | function\<void()\> |
| `<unistd.h>` | read write close |
| `<fcntl.h>` | fcntl O_NONBLOCK |
| `<cerrno>` | errno EAGAIN |
| `<sys/epoll.h>` | epoll_* |
| `<sys/socket.h>` | socket bind listen accept setsockopt |
| `<netinet/in.h>` | sockaddr_in htons |
| `<arpa/inet.h>` | 常与 in.h 一起 |

### 四个常量（main 和全局会用）

```cpp
const string WEB_ROOT = "www";
const int MAX_EVENTS = 64;
const int PORT = 8080;
const size_t THREAD_POOL_SIZE = 4;
```

---

# 默写篇

---

## 六、默写顺序（推荐 12 步）

**不要从 main 写起。** 按下面顺序，每步写完能编译更好（最后链接 pthread）。

| 步 | 写什么 | 记忆钩子 |
|----|--------|---------|
| **1** | 全部 `#include` + `using namespace std` + 4 个常量 | iostream 到 arpa，线程库在中间 |
| **2** | `struct HttpRequest` + `parseRequestLine` | find → substr → iss >> 三个 |
| **3** | `buildHttpResponse` | HTTP/1.1 + Content-Type + Length + 空行 + body |
| **4** | `pathToFile` + `isPathSafe` | / → index.html；无 `..` |
| **5** | `getContentType` + `readFile` | .html .css .js .png .jpg + ifstream assign |
| **6** | `buildResponseFromRequest` | GET? safe? read? 200 |
| **7** | `processRawRequest` | parse 失败 400；成功调 6 |
| **8** | `class ThreadPool` 整类 | 构造 workerLoop submit 析构 + 5 成员 |
| **9** | `setNonBlocking` + `addFd` + `removeFd` | fcntl；EPOLLIN；DEL |
| **10** | `dispatchClient` | read removeFd submit lambda |
| **11** | `main` 上半：pool + socket bind listen epoll | 和 Step4 一样 |
| **12** | `main` 下半：while epoll_wait 两分支 | listen→accept；else→dispatch |

---

## 七、分块模板：每块要写什么

### 块 1：HttpRequest + parseRequestLine

```cpp
struct HttpRequest { string method; string path; string version; };

bool parseRequestLine(const string& raw, HttpRequest& req) {
    // 1. find "\r\n"，没有则 find '\n'，还没有 return false
    // 2. line = substr(0, lineEnd)
    // 3. istringstream iss(line);
    // 4. iss >> req.method >> req.path >> req.version，失败 return false
    // 5. return true
}
```

### 块 2：buildHttpResponse

```cpp
// 返回拼接字符串：
// "HTTP/1.1 " + statusCode + " " + statusText + "\r\n"
// "Content-Type: " + contentType + "\r\n"
// "Content-Length: " + to_string(body.size()) + "\r\n"
// "Connection: close\r\n"
// "\r\n" + body
```

### 块 3：静态文件四件套

```cpp
pathToFile:  / 或 /index.html → WEB_ROOT+"/index.html"；否则 WEB_ROOT+urlPath
isPathSafe:  find("..") == npos
getContentType: 末尾 .html .css .js .png .jpg（注意 size>=N 再 substr）
readFile:  ifstream binary → out.assign(istreambuf_iterator...)
```

### 块 4：buildResponseFromRequest

```cpp
// 顺序固定，便于记：
if (method != "GET")     → 405
if (!isPathSafe(path))   → 403   ← 注意是 !isPathSafe
filePath = pathToFile(path)
if (!readFile(...))      → 404
return 200 + getContentType + body
```

### 块 5：processRawRequest（Step5 新增）

```cpp
// parse 失败 → 400
// 否则 cout worker id + method path
// return buildResponseFromRequest(req)
```

### 块 6：ThreadPool（最长，分四段记）

**成员变量（5 个）：**

```cpp
queue<function<void()>> tasks_;
mutex mtx_;
condition_variable cv_;
vector<thread> workers_;
bool stop_;
```

**构造：**

```cpp
explicit ThreadPool(size_t n) : stop_(false) {
    for (size_t i = 0; i < n; ++i)
        workers_.emplace_back([this]() { workerLoop(); });
}
```

**submit：**

```cpp
{ lock_guard; tasks_.push(move(task)); }
cv_.notify_one();
```

**workerLoop：**

```cpp
while(true) {
    unique_lock + cv_.wait( stop_ || !tasks_.empty() )
    if (stop_ && tasks_.empty()) return;
    task = move(tasks_.front()); tasks_.pop();
    // 解锁后
    task();
}
```

**析构：**

```cpp
{ lock_guard; stop_=true; }
cv_.notify_all();
for (t : workers_) if (t.joinable()) t.join();
```

### 块 7：epoll 三函数

```cpp
setNonBlocking:  F_GETFL → F_SETFL | O_NONBLOCK
addFd:  epoll_event ev; ev.events=EPOLLIN; ev.data.fd=fd; epoll_ctl ADD
removeFd:  epoll_ctl DEL, nullptr
```

### 块 8：dispatchClient

```cpp
// 1. read buffer
// 2. removeFd(epfd, client_fd)   ← 在 submit 前
// 3. rawRequest.assign(buffer, n) if n>0
// 4. pool.submit([client_fd, rawRequest, n]() {
//      n<=0 → 400 else processRawRequest
//      write; close; cout 完成
//    })
```

---

## 八、main 函数逐步清单

默写 `main` 时按勾选顺序写，**不要跳步**：

```text
□ ThreadPool pool(THREAD_POOL_SIZE);

□ listen_fd = socket(AF_INET, SOCK_STREAM, 0)
□ setsockopt SO_REUSEADDR
□ sockaddr_in: AF_INET, INADDR_ANY, htons(PORT)
□ bind → listen(128)
□ setNonBlocking(listen_fd)

□ epfd = epoll_create1(0)
□ addFd(epfd, listen_fd)

□ vector<epoll_event> events(MAX_EVENTS);

□ while (true) {
      nready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1)
      if (nready < 0) continue;

      for (i = 0; i < nready; ++i) {
          fd = events[i].data.fd

          if (fd == listen_fd) {
              while (true) {
                  client_fd = accept(...)
                  if (client_fd < 0) {
                      if (errno == EAGAIN || EWOULDBLOCK) break;
                      break;
                  }
                  setNonBlocking(client_fd);
                  addFd(epfd, client_fd);
              }
          } else {
              dispatchClient(epfd, fd, pool);
          }
      }
  }
```

---

## 九、易错点检查表

默写完成后逐项检查：

| # | 检查项 | 正确写法 |
|---|--------|---------|
| 1 | 路径安全 | `if (!isPathSafe(req.path))` → 403 |
| 2 | HTTP 版本 | 响应行 `HTTP/1.1`（不是 Http/1.1） |
| 3 | Content-Length | 用 `body.size()` 自动算 |
| 4 | isPathSafe 含义 | true=安全，不安全才 403 |
| 5 | accept 循环 | 非阻塞 listen 要 while accept 到 EAGAIN |
| 6 | removeFd 时机 | read 后、submit 前，不是 close 后 |
| 7 | lambda 捕获 | `[client_fd, rawRequest, n]` 按值捕获 |
| 8 | epoll 只在主线程 | worker 不调用 epoll_wait |
| 9 | 编译 | `g++ -std=c++17 -Wall -pthread -o server Step1to6/Step5.cpp`（项目根目录） |
| 10 | 运行目录 | 项目根目录（含 `www/`），执行 `./server` |

---

## 十、完整 Step5.cpp 源码（对照用）

> 默写完成后与此对照。建议先关其他文件，凭记忆写，再打开本节纠错。

```cpp
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
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

struct HttpRequest {
    string method;
    string path;
    string version;
};

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

string processRawRequest(const string& rawRequest) {
    HttpRequest req;
    if (!parseRequestLine(rawRequest, req))
        return buildHttpResponse(400, "Bad Request",
            "text/plain; charset=utf-8", "请求行格式错误");
    cout << "[worker " << this_thread::get_id() << "] "
         << req.method << " " << req.path << "\n";
    return buildResponseFromRequest(req);
}

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

void dispatchClient(int epfd, int client_fd, ThreadPool& pool) {
    char buffer[8192] = {0};
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    removeFd(epfd, client_fd);

    string rawRequest;
    if (n > 0) rawRequest.assign(buffer, n);

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

int main() {
    ThreadPool pool(THREAD_POOL_SIZE);

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

    cout << "线程池服务器已启动：http://127.0.0.1:" << PORT << "\n";
    cout << "工作线程数: " << THREAD_POOL_SIZE << "\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";

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

## 附录 A：30 秒背诵口诀

```text
开店 socket bind listen，
解析 find substr iss，
响应 HTTP 头加 body，
静态 www path 读盘，
epoll 等非阻塞 accept，
线程池 queue mutex cv，
主读工人写关 fd。
```

---

## 附录 B：编译与运行

在**项目根目录**执行：

```bash
g++ -std=c++17 -Wall -pthread -o server Step1to6/Step5.cpp
./server
```

浏览器：`http://127.0.0.1:8080/`（需项目根目录有 `www/index.html`；源码在 `Step1to6/Step5.cpp`）

---

## 附录 C：建议练习方式

| 阶段 | 做法 |
|------|------|
| 第 1 天 | 只默写 Step1 部分（main 里 accept read write 固定响应） |
| 第 2 天 | 默写到 Step2（加 parse + buildHttpResponse） |
| 第 3 天 | 默写到 Step3（加静态文件 4 函数） |
| 第 4 天 | 默写到 Step4（改 main 为 epoll + handleClient 版） |
| 第 5 天 | 完整 Step5（ThreadPool + dispatchClient） |
| 第 6 天 | 闭卷 45 分钟内写完，再用第十节对照 |

---

> **相关文档（深入某一步时用）：**  
> [01 指南](./01-最小可行Web服务器指南.md) · [02 指南](./02-解析HTTP请求指南.md) · [03 指南](./03-返回静态文件指南.md) · [04 指南](./04-epoll高并发指南.md) · [05 指南](./05-线程池指南.md)
