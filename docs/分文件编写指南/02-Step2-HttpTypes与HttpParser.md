# 分文件 Step2 —— HttpTypes、HttpParser、HttpResponse、HttpHandler（详细教程）

> **写给谁看：** 已完成 [分文件 Step1](./01-Step1-main最小服务器.md) 的学习者——`src/main.cpp` 能跑、浏览器能收到固定 `Hello World!`。  
> **本篇目标：** 把「解析请求行」「拼 HTTP 响应」「按路径路由」从 `main` 拆到独立模块；`main` 只保留 socket 主循环。  
> **不做的事：** 不读磁盘静态文件（Step3）、不用 epoll（Step4）、不用多线程（Step5）。  
> **前置文档：** [00-总览与工程结构](./00-总览与工程结构.md) · [01-Step1-main最小服务器](./01-Step1-main最小服务器.md) · [02-解析HTTP请求指南](../02-解析HTTP请求指南.md)  
> **单文件对照：** [Step1to6/Step2.cpp](../../Step1to6/Step2.cpp)

---

## 目录

1. [第一部分：架构位置、为什么要拆、生活类比](#第一部分架构位置为什么要拆生活类比)
2. [第二部分：本步文件树（前后对比）](#第二部分本步文件树前后对比)
3. [第三部分：每个文件的完整代码](#第三部分每个文件的完整代码)
4. [第四部分：逐文件 / 逐块讲解](#第四部分逐文件--逐块讲解)
5. [Makefile 完整说明](#makefile-完整说明)
6. [编译、运行与测试](#编译运行与测试)
7. [常见问题排查](#常见问题排查)
8. [与单文件 Step2.cpp 对照表](#与单文件-step2cpp-对照表)
9. [本篇小结与下一篇链接](#本篇小结与下一篇链接)

---

## 第一部分：架构位置、为什么要拆、生活类比

### 1.1 本篇在架构中的位置

Step1 把所有逻辑塞进 `main.cpp`。Step2 是**第一次真正的模块化拆分**：按「职责」把代码搬到不同文件，但**运行模型不变**——仍是阻塞 `accept`、读一次、回一次、关闭。

```text
                    main.cpp
                  （socket 主循环）
                       │
           read 原始字节 ──┐
                           ▼
                    HttpParser
                  parseRequestLine
                           │
                     HttpRequest
                           │
                           ▼
                    HttpHandler
                    handleRequest
                           │
                           ▼
                    HttpResponse
                  buildHttpResponse
                           │
                     完整 HTTP 字符串
                           │
                           ▼
                    main: write → close
```

### 1.2 为什么要拆？—— 单文件 Step2 的痛点

单文件 `Step2.cpp` 约 195 行，已经出现 4 类不同职责：

| 职责 | 单文件里的形态 | 拆出去后 |
|------|---------------|---------|
| 数据结构 | `struct HttpRequest` | `HttpTypes.h` |
| 解析请求行 | `parseRequestLine()` | `HttpParser.cpp` |
| 拼响应头 + 正文 | `buildHttpResponse()` | `HttpResponse.cpp` |
| 业务路由 | `handleRequest()` | `HttpHandler.cpp` |
| 网络 I/O | `main` 里 socket 循环 | 仍留 `main.cpp` |

若继续堆在 `main.cpp`，会出现：

- 改 HTML 路由要翻几千行 socket 代码（Step3 后更多）
- 多人协作时所有人改同一文件，冲突频繁
- 单元测试无法单独测 `parseRequestLine`

**生活类比：** Step1 是「一个人又接电话又做菜又结账」。Step2 是「前台只接电话，后厨按菜单做菜，收银只负责打包账单格式」——客人体验不变，但分工清晰。

### 1.3 第二篇单文件 vs 分文件 Step2：能力对齐

| 能力 | 单文件 Step2 | 分文件 Step2（本篇） |
|------|-------------|---------------------|
| 解析请求行 | 有 | 有（`HttpParser`） |
| GET 路由 `/`、`/about`、`/hello` | 有 | 有（`HttpHandler`） |
| 405 / 404 / 400 | 有 | 有 |
| 读 `www/` 磁盘 | 无 | 无（Step3） |
| HTML 存在哪 | **C++ 字符串里** | 相同 |

### 1.4 HTTP 请求行回顾

浏览器访问 `http://127.0.0.1:8080/about` 时，请求报文第一行：

```http
GET /about HTTP/1.1
```

| 字段 | 示例 | 本篇用途 |
|------|------|---------|
| Method | `GET` | 非 GET 返回 405 |
| Path | `/about` | `handleRequest` 路由键 |
| Version | `HTTP/1.1` | 解析存入 `req.version`，路由暂不用 |

### 1.5 本篇数据流（一次请求）

```text
  accept → read(buffer)
            │
            ▼
  parseRequestLine(buffer, req)  ──失败──► 400 Bad Request
            │ 成功
            ▼
  handleRequest(req)
     ├─ method != GET ──► 405
     ├─ path == "/" ──► 200 HTML 首页
     ├─ path == "/about" ──► 200 纯文本
     ├─ path == "/hello" ──► 200 纯文本
     └─ 其它 ──► 404
            │
            ▼
  write(response) → close(client_fd)
```

---

## 第二部分：本步文件树（前后对比）

### 2.1 Step1 结束时

```text
MyWebServer/
├── src/
│   └── main.cpp           ← 全是 socket + 固定 Hello World
├── Makefile
└── Step1to6/Step2.cpp     ← 单文件对照（保留）
```

### 2.2 Step2 结束时

```text
MyWebServer/
├── src/
│   ├── main.cpp           ← 【改写】只保留 socket 主循环 + 调用 Parser/Handler
│   ├── HttpTypes.h        ← 【新建】HttpRequest 结构体
│   ├── HttpParser.h       ← 【新建】
│   ├── HttpParser.cpp     ← 【新建】parseRequestLine 实现
│   ├── HttpResponse.h     ← 【新建】
│   ├── HttpResponse.cpp   ← 【新建】buildHttpResponse 实现
│   ├── HttpHandler.h      ← 【新建】
│   └── HttpHandler.cpp    ← 【新建】handleRequest 路由
├── Makefile               ← 【更新】链接多个 .cpp
└── www/                   ← 仍未使用（Step3）
```

### 2.3 本步操作清单

| 操作 | 文件 |
|------|------|
| 新建 | `HttpTypes.h`, `HttpParser.h/cpp`, `HttpResponse.h/cpp`, `HttpHandler.h/cpp` |
| 改写 | `src/main.cpp` |
| 更新 | `Makefile` |

---

## 第三部分：每个文件的完整代码

> 以下 8 个源文件 + Makefile 均可整段复制，**无省略号**。

### 3.1 `src/HttpTypes.h`

```cpp
#pragma once

#include <string>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};
```

### 3.2 `src/HttpParser.h`

```cpp
#pragma once

#include "HttpTypes.h"
#include <string>

bool parseRequestLine(const std::string& raw, HttpRequest& req);
```

### 3.3 `src/HttpParser.cpp`

```cpp
#include "HttpParser.h"

#include <sstream>

using namespace std;

bool parseRequestLine(const string& raw, HttpRequest& req) {
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == string::npos) {
        lineEnd = raw.find('\n');
    }
    if (lineEnd == string::npos) {
        return false;
    }

    string line = raw.substr(0, lineEnd);

    istringstream iss(line);
    if (!(iss >> req.method >> req.path >> req.version)) {
        return false;
    }
    return true;
}
```

### 3.4 `src/HttpResponse.h`

```cpp
#pragma once

#include <string>

std::string buildHttpResponse(int statusCode,
                              const std::string& statusText,
                              const std::string& contentType,
                              const std::string& body);
```

### 3.5 `src/HttpResponse.cpp`

```cpp
#include "HttpResponse.h"

#include <string>

using namespace std;

string buildHttpResponse(int statusCode,
                         const string& statusText,
                         const string& contentType,
                         const string& body) {
    return "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" +
           body;
}
```

### 3.6 `src/HttpHandler.h`

```cpp
#pragma once

#include "HttpTypes.h"
#include <string>

std::string handleRequest(const HttpRequest& req);
```

### 3.7 `src/HttpHandler.cpp`

```cpp
#include "HttpHandler.h"
#include "HttpResponse.h"

using namespace std;

string handleRequest(const HttpRequest& req) {
    if (req.method != "GET") {
        return buildHttpResponse(
            405, "Method Not Allowed",
            "text/plain; charset=utf-8",
            "暂只支持 GET 请求，收到的是: " + req.method);
    }

    if (req.path == "/" || req.path == "/index.html") {
        string body =
            "<!DOCTYPE html>\r\n"
            "<html><head><meta charset=\"utf-8\"><title>首页</title></head>\r\n"
            "<body>\r\n"
            "<h1>欢迎访问 MyWebServer</h1>\r\n"
            "<p>试试这些链接：</p>\r\n"
            "<ul>\r\n"
            "<li><a href=\"/about\">/about</a> —— 关于页</li>\r\n"
            "<li><a href=\"/hello\">/hello</a> —— 问候页</li>\r\n"
            "<li><a href=\"/not-exist\">/not-exist</a> —— 故意触发 404</li>\r\n"
            "</ul>\r\n"
            "</body></html>\r\n";
        return buildHttpResponse(200, "OK", "text/html; charset=utf-8", body);
    }

    if (req.path == "/about") {
        return buildHttpResponse(
            200, "OK", "text/plain; charset=utf-8",
            "这是关于页面。\nMyWebServer 是一个学习用的轻量级 Web 服务器。");
    }

    if (req.path == "/hello") {
        return buildHttpResponse(
            200, "OK", "text/plain; charset=utf-8",
            "Hello! 路由生效了，你访问的是 /hello");
    }

    return buildHttpResponse(
        404, "Not Found", "text/plain; charset=utf-8",
        "404 - 页面不存在: " + req.path);
}
```

### 3.8 `src/main.cpp`（完整改写版）

```cpp
#include <iostream>
#include <algorithm>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "HttpParser.h"
#include "HttpHandler.h"
#include "HttpResponse.h"

using namespace std;

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "socket 创建失败\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "bind 失败（端口可能被占用）\n";
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        cerr << "listen 失败\n";
        close(server_fd);
        return 1;
    }

    cout << "Step2 分文件版已启动：http://127.0.0.1:8080\n";
    cout << "在浏览器打开上述地址，或用 curl 测试\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            cerr << "accept 失败\n";
            continue;
        }

        cout << "收到新连接\n";

        char buffer[4096] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

        string response;
        if (n <= 0) {
            response = buildHttpResponse(
                400, "Bad Request", "text/plain; charset=utf-8",
                "无法读取请求");
        } else {
            string rawRequest(buffer, n);
            cout << "收到请求（前 120 字符）：\n"
                 << rawRequest.substr(0, min(rawRequest.size(), size_t(120))) << "\n";

            HttpRequest req;
            if (!parseRequestLine(rawRequest, req)) {
                response = buildHttpResponse(
                    400, "Bad Request", "text/plain; charset=utf-8",
                    "请求行格式错误，期望: GET /path HTTP/1.1");
            } else {
                cout << "解析结果: " << req.method << " " << req.path
                     << " " << req.version << "\n";
                response = handleRequest(req);
            }
        }

        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "已响应并关闭连接\n";
    }

    close(server_fd);
    return 0;
}
```

> **注意：** `main` 在「读失败 / 解析失败」时直接调用 `buildHttpResponse` 返回 400，因此必须 `#include "HttpResponse.h"`（`HttpHandler.h` 不会替你转包含）。

### 3.9 `Makefile`（完整）

```makefile
# MyWebServer 分文件工程 —— Step2
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Isrc

SRCS = src/main.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp

.PHONY: all clean

all: server

server: $(SRCS)
	$(CXX) $(CXXFLAGS) -o server $(SRCS)

clean:
	rm -f server
```

---

## 第四部分：逐文件 / 逐块讲解

### 4.1 模块依赖关系

```text
HttpTypes.h          （无依赖，纯数据结构）
    ▲
    ├── HttpParser.h / .cpp
    ├── HttpHandler.h / .cpp
    │
HttpResponse.h / .cpp  （独立工具函数，Step2 不依赖 HttpTypes）
    ▲
    └── HttpHandler.cpp 调用 buildHttpResponse
            ▲
            └── main.cpp 调用 Parser、Handler、Response
```

| 文件 | 依赖的头文件 | 被谁使用 |
|------|-------------|---------|
| `HttpTypes.h` | `<string>` | Parser、Handler |
| `HttpParser` | `HttpTypes.h` | `main.cpp` |
| `HttpResponse` | 无项目内依赖 | Handler、`main.cpp` |
| `HttpHandler` | `HttpTypes.h`, `HttpResponse.h` | `main.cpp` |
| `main.cpp` | Parser, Handler, Response | 入口 |

### 4.2 `HttpTypes.h` —— 公共数据契约

```cpp
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};
```

| 字段 | 含义 | 本篇是否用于路由 |
|------|------|-----------------|
| `method` | HTTP 方法，如 `GET` | 是（非 GET → 405） |
| `path` | URL 路径，如 `/about` | 是（路由键） |
| `version` | 协议版本 | 仅解析存储，路由不用 |

**为何单独成头文件？** Step6 状态机、Step8 `HttpConnection` 都要传 `HttpRequest`。集中定义避免「Parser 里一套、Handler 里另一套」。

**`.h` 里放 `struct` 可以吗？** 可以。纯数据、无链接符号，多个 cpp include 不会产生 `multiple definition`。

### 4.3 `HttpParser` —— 只解析第一行

#### `parseRequestLine` 步骤表

| 步骤 | 代码 | 失败时 |
|------|------|--------|
| 1 | `raw.find("\r\n")` 找行尾 | 若无则试 `\n` |
| 2 | 都没有行尾 | `return false` → main 回 400 |
| 3 | `substr(0, lineEnd)` 取第一行 | — |
| 4 | `istringstream >> method >> path >> version` | 缺字段则 `false` |

**为何用 `istringstream`？** 请求行格式是「空格分隔的三个 token」，流提取比手写 `find(' ')` 更短、更不易错。

**本篇不解析什么？**

- 请求头（`Host:`、`User-Agent:`）
- 请求体（POST body）
- 多行 path（带 query 的 `?a=1` 会整段进 `path`，Step6+ 再细化）

### 4.4 `HttpResponse` —— 拼完整 HTTP 报文

```cpp
return "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
       ...
       "\r\n" + body;
```

| 部分 | 说明 |
|------|------|
| 状态行 | `HTTP/1.1 200 OK` —— 注意 `HTTP` 全大写（单文件 Step2 写成 `Http/1.1` 不规范，分文件版已修正） |
| `Content-Type` | 告诉浏览器如何解释正文 |
| `Content-Length` | `body.size()` 字节数，**必须用 `string::size()`**，不能心算 |
| `Connection: close` | 简单模型：一次响应后关连接 |
| 空行 + body | 头体分隔 |

**为何单独模块？** Step3 会在同文件加 `readFile`、`getContentType`；Step10 加 302 重定向。响应相关逻辑聚合在 `HttpResponse` 最自然。

### 4.5 `HttpHandler` —— 业务路由（仍用内存字符串）

| 条件 | 状态码 | 正文类型 |
|------|--------|---------|
| `method != "GET"` | 405 | 纯文本 |
| `path == "/"` 或 `/index.html` | 200 | HTML（内嵌在 cpp 字符串） |
| `path == "/about"` | 200 | 纯文本 |
| `path == "/hello"` | 200 | 纯文本 |
| 其它 | 404 | 纯文本 |

**生活类比：** `HttpHandler` 是「点菜员」——看客人要的 path，决定上什么菜。  
`HttpResponse` 是「打包员」——不管菜是什么，都包成标准 HTTP 盒子。

**Step3 会怎么改 Handler？** 删掉大段 HTML 字符串，改成 `readFile(www/...)`。接口 `handleRequest(const HttpRequest&)` **不变**，main 不用改调用方式。

### 4.6 `main.cpp` —— 瘦身后职责

| 仍由 main 做 | 拆出去后不做 |
|-------------|-------------|
| `socket` / `bind` / `listen` / `accept` | 解析请求行 |
| `read` / `write` / `close` | 拼 HTTP 响应 |
| 调用 `parseRequestLine`、`handleRequest` | 路由 if-else |
| 读失败 / 解析失败时调 `buildHttpResponse(400,...)` | — |

主循环核心片段：

```cpp
HttpRequest req;
if (!parseRequestLine(rawRequest, req)) {
    response = buildHttpResponse(400, ...);
} else {
    response = handleRequest(req);
}
```

这就是 **「main 管 I/O，Handler 管业务」** 的经典分层，后续 epoll 只是把 `read/write` 搬进 `HttpConnection`，分层不变。

### 4.7 `.h` / `.cpp` 拆分约定（复习）

| 放在 `.h` | 放在 `.cpp` |
|-----------|------------|
| 函数**声明** | 函数**定义** |
| `struct` / `enum` | `using namespace std;`（尽量只放 cpp） |
| `#pragma once` | `#include` 具体实现需要的头 |

**链接规则：** `Makefile` 必须把每个含定义的 `.cpp` 都列进 `SRCS`，否则报 `undefined reference to parseRequestLine`。

---

## Makefile 完整说明

### 5.1 与 Step1 的差异

| 项目 | Step1 | Step2 |
|------|-------|-------|
| 源文件数 | 1 | 4 个 cpp |
| `SRCS` | 无（直接写路径） | 列出全部 cpp |
| 链接 | 单文件编译即链接 | 多 `.o` 隐式链接（g++ 一步完成） |

### 5.2 编译命令等价形式

`make` 实际执行：

```bash
g++ -std=c++17 -Wall -Isrc -o server \
    src/main.cpp \
    src/HttpParser.cpp \
    src/HttpResponse.cpp \
    src/HttpHandler.cpp
```

`-Isrc` 保证 `#include "HttpParser.h"` 能在 `src/` 目录找到。

### 5.3 常见 Makefile 漏写后果

| 漏写文件 | 报错 |
|---------|------|
| `HttpParser.cpp` | `undefined reference to parseRequestLine` |
| `HttpHandler.cpp` | `undefined reference to handleRequest` |
| `HttpResponse.cpp` | `undefined reference to buildHttpResponse` |

---

## 编译、运行与测试

### 6.1 编译

```bash
cd /path/to/MyWebServer
make clean
make
```

**预期：** 无 error；生成 `./server`。

### 6.2 启动

```bash
./server
```

**预期输出：**

```text
Step2 分文件版已启动：http://127.0.0.1:8080
在浏览器打开上述地址，或用 curl 测试
```

### 6.3 测试用例与预期结果

#### 测试 1：首页 HTML

```bash
curl -s http://127.0.0.1:8080/
```

**预期：** 含 `<h1>欢迎访问 MyWebServer</h1>` 和三个链接。

#### 测试 2：/about

```bash
curl -s http://127.0.0.1:8080/about
```

**预期：**

```text
这是关于页面。
MyWebServer 是一个学习用的轻量级 Web 服务器。
```

#### 测试 3：/hello

```bash
curl -s http://127.0.0.1:8080/hello
```

**预期：**

```text
Hello! 路由生效了，你访问的是 /hello
```

#### 测试 4：404

```bash
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/not-exist
```

**预期：** `404`，正文含 `404 - 页面不存在: /not-exist`

#### 测试 5：405（用 curl 发 POST）

```bash
curl -s -o /dev/null -w "%{http_code}" -X POST http://127.0.0.1:8080/
```

**预期：** `405`

#### 测试 6：终端日志

访问任意 URL 后，服务器终端应打印：

```text
收到新连接
收到请求（前 120 字符）：
GET /about HTTP/1.1
...
解析结果: GET /about HTTP/1.1
已响应并关闭连接
```

### 6.4 浏览器测试

打开 `http://127.0.0.1:8080/`，点击「关于页」「问候页」「故意 404」链接，分别验证内容与状态。

### 6.5 验收清单

| # | 检查项 | 通过标准 |
|---|--------|---------|
| 1 | `make` | 无编译 / 链接错误 |
| 2 | `/` | HTML 首页 |
| 3 | `/about`、`/hello` | 对应纯文本 |
| 4 | 不存在路径 | 404 |
| 5 | POST | 405 |
| 6 | 模块文件 | `src/` 下 8 个文件齐全 |

---

## 常见问题排查

### Q1：`undefined reference to parseRequestLine`

**原因：** `Makefile` 未加入 `HttpParser.cpp`。  
**办法：** 核对 `SRCS` 四个 cpp 齐全，重新 `make`。

### Q2：`buildHttpResponse was not declared in this scope`（main.cpp）

**原因：** `main.cpp` 在 400 分支调用了 `buildHttpResponse` 但未 `#include "HttpResponse.h"`。  
**办法：** 按 3.8 节说明补上 include。

### Q3：`multiple definition of parseRequestLine`

**原因：** 把函数**定义**写在 `.h` 里，又被多个 cpp include。  
**办法：** 定义只放 `HttpParser.cpp`；`.h` 只保留声明。

### Q4：浏览器显示乱码

**检查：** HTML 是否含 `<meta charset="utf-8">`；`Content-Type` 是否带 `charset=utf-8`。

### Q5：访问 `/about` 返回 404

**检查：**

- 路径是否大小写一致（`/About` 不匹配）
- `parseRequestLine` 是否成功（看终端「解析结果」）
- `HttpHandler.cpp` 是否保存、是否重新 `make`

### Q6：单文件 Step2 能跑，分文件版 404 全文不对

对照 [第八节对照表](#与单文件-step2cpp-对照表)，确认 `HTTP/1.1` 大小写、`Content-Length` 由 `body.size()` 自动计算。

### Q7：为什么要 `extern` 或拆 `ServerConfig`？

Step2 **不需要**全局配置。`web_root` 在 Step3 才通过 `ServerConfig` 引入。若提前引入，反而增加初学负担。

---

## 与单文件 Step2.cpp 对照表

| 代码块 | 单文件 `Step2.cpp` 位置 | 分文件位置 |
|--------|------------------------|-----------|
| `struct HttpRequest` | 第 14～19 行 | `HttpTypes.h` |
| `parseRequestLine` | 第 22～45 行 | `HttpParser.cpp` |
| `buildHttpResponse` | 第 48～61 行 | `HttpResponse.cpp` |
| `handleRequest` | 第 64～111 行 | `HttpHandler.cpp` |
| `main` socket 循环 | 第 113～195 行 | `main.cpp` |
| 启动日志 | `服务器已启动` | `Step2 分文件版已启动`（便于区分） |
| 状态行大小写 | `Http/1.1`（笔误） | `HTTP/1.1`（已修正） |
| `Connection: close` | 有 | 有 |
| 路由表 | `/`、`/about`、`/hello` | 相同 |
| 读磁盘 | 无 | 无 |

**逻辑等价性：** 行为应与单文件 Step2 一致（修正 HTTP 大小写后浏览器更规范）。验收以 curl 测试表为准。

---

## 本篇小结与下一篇链接

### 你现在已经会了什么

- [x] 按职责把单文件 Step2 拆成 4 模块 + 瘦 `main`
- [x] 用 `HttpTypes.h` 统一请求数据结构
- [x] `HttpParser` 解析请求行，`HttpHandler` 做内存路由
- [x] `HttpResponse` 统一拼 HTTP 响应头
- [x] 多文件 `Makefile` 链接通过

### 拆文件后的收益（已为 Step3 准备好）

| 模块 | Step3 将做的扩展 |
|------|-----------------|
| `HttpResponse` | 增加 `readFile`、`pathToFile`、`getContentType` |
| `HttpHandler` | 删除 HTML 大字符串，改为读 `www/` |
| `HttpParser` | 暂不变 |
| `main.cpp` | 暂基本不变 |

### 建议实验

1. 在 `HttpHandler` 增加路由 `/time`，返回当前时间字符串——体会「只改 Handler、不动 main」。
2. 故意把 `parseRequestLine` 改成只认 `\r\n` 不认 `\n`，用 `curl` 看 400——体会解析健壮性。

---

> **下一步：** 进入 [03-Step3-HttpResponse静态文件](./03-Step3-HttpResponse静态文件.md)，引入 `ServerConfig` 与 `www/` 目录，让页面从磁盘读取，不再写在 C++ 字符串里。
