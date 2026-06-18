# 解析 HTTP 请求 —— 零基础学习指南（第二篇）

> **写给谁看：** 已完成 [第一篇](./01-最小可行Web服务器指南.md) 的学习者——服务器能跑、浏览器能收到固定 `Hello World`。  
> **本篇目标：** 读懂浏览器发来的 HTTP 请求，根据 **URL 路径** 返回不同内容。  
> **不做的事：** 不读静态文件、不用 epoll、不用多线程 —— 这些留到后面的篇章。  
> **前置文档：** [01-最小可行Web服务器指南](./01-最小可行Web服务器指南.md) · [02-第一步代码深度答疑](./02-第一步代码深度答疑.md)（可选）

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

### 1.1 第一篇 vs 第二篇：差在哪？

| 对比项 | 第一篇（最小 Demo） | 第二篇（本篇） |
|--------|-------------------|---------------|
| 读请求 | 读进来打印，**不分析** | 解析**请求行**，提取 Method 和 Path |
| 写响应 | **固定** `Hello World` | 根据 **URL 路径** 返回不同页面 |
| 404 | 没有 | 访问不存在的路径返回 **404** |
| 生活类比 | 客人说啥，你都回同一句"欢迎" | 客人说"我要菜单/关于我们/结账"，你分别处理 |

### 1.2 HTTP 请求长什么样？

浏览器访问 `http://127.0.0.1:8080/about` 时，发到服务器的原始文本大致如下：

```http
GET /about HTTP/1.1
Host: 127.0.0.1:8080
User-Agent: Mozilla/5.0 ...
Accept: text/html,application/xhtml+xml,...
Accept-Language: zh-CN,zh;q=0.9
Connection: keep-alive

```

结构分成三块：

```text
┌─────────────────────────────────────┐
│  请求行（Request Line）  ← 本篇重点  │  GET /about HTTP/1.1
├─────────────────────────────────────┤
│  请求头（Headers）      ← 本篇先跳过  │  Host: ...
│                                     │  User-Agent: ...
├─────────────────────────────────────┤
│  空行 \r\n                           │
├─────────────────────────────────────┤
│  请求体（Body）         ← GET 通常没有 │  （空）
└─────────────────────────────────────┘
```

**本篇只解析第一行（请求行）**，就够实现"不同 URL 不同内容"。

### 1.3 请求行：三个词，用空格分开

```http
GET /about HTTP/1.1
│   │      │
│   │      └── 协议版本（HTTP/1.1）
│   └────────── 路径（Path）—— 我们要用来"路由"
└────────────── 方法（Method）—— 常见 GET、POST
```

| 字段 | 本篇示例 | 含义 |
|------|---------|------|
| Method | `GET` | 客户端想**获取**资源（浏览器地址栏默认就是 GET） |
| Path | `/about` | 要访问的路径，类似网站里的 `/home`、`/login` |
| Version | `HTTP/1.1` | 使用的 HTTP 版本 |

**生活类比：** 请求行就像客人进门说的第一句话——  
**"我要（GET）看关于我们（/about）的页面"**。  
服务员（服务器）听到 **Path**，决定去哪个货架取货。

### 1.4 路由：根据 Path 决定返回什么

"路由"（Routing）不是新语法，就是 **`if / else if` 判断路径**：

```text
Path 是 /        → 返回首页 HTML
Path 是 /about   → 返回"关于我们"纯文本
Path 是 /hello   → 返回 Hello 文本
其他路径         → 返回 404 Not Found
```

### 1.5 本篇程序的整体流程

```text
  （第一篇已有：socket → bind → listen）
     │
     ▼
  accept 接受连接
     │
     ▼
  read 读取原始请求
     │
     ▼
  【新增】解析请求行 → 得到 method、path
     │
     ▼
  【新增】根据 path 选择响应内容
     │
     ▼
  【新增】自动拼 HTTP 响应（含正确的 Content-Length）
     │
     ▼
  write 发送 → close 关闭
     │
     └────── 循环
```

---

## 第二部分：需要哪些新"工具"，为什么要用

### 2.1 新增头文件

| 头文件 | 提供什么 | 为什么需要 |
|--------|---------|-----------|
| `<string>` | `std::string` | 方便截取、拼接 HTTP 文本（比 `char*` 安全） |
| `<sstream>` | `std::istringstream` | 把一行文字按空格"切"成 method、path、version |

第一篇已有的头文件**全部保留**，socket 部分不变。

### 2.2 新增数据结构：`HttpRequest`

把解析结果装进一个"小盒子"，后面代码更清晰：

```cpp
struct HttpRequest {
    string method;   // 如 "GET"
    string path;     // 如 "/about"
    string version;  // 如 "HTTP/1.1"
};
```

**为什么不用三个独立变量？** 可以，但结构体把"一次请求的关键信息"绑在一起，函数传参更方便。

### 2.3 三个新函数（分工明确）

| 函数 | 职责 | 生活类比 |
|------|------|---------|
| `parseRequestLine()` | 从原始报文里**抠出**请求行，填进 `HttpRequest` | 听客人说第一句，记下"要什么、去哪" |
| `handleRequest()` | 根据 path **决定**返回什么内容 | 厨师按菜单做菜 |
| `buildHttpResponse()` | 把正文**包装**成标准 HTTP 响应 | 把菜装盒、贴标签（Content-Length 等） |

### 2.4 用到的 C++ 字符串操作（提前认识）

| 操作 | 代码示例 | 作用 |
|------|---------|------|
| 找子串位置 | `raw.find("\r\n")` | 找到第一行结束的位置 |
| 截取子串 | `raw.substr(0, lineEnd)` | 取出第一行文字 |
| 按空格切分 | `iss >> method >> path >> version` | 解析 `GET /about HTTP/1.1` |
| 转字符串 | `to_string(body.size())` | 把数字变成文本，拼进响应头 |
| 字节长度 | `body.size()` | **Content-Length 必须用字节数**（中文也适用） |

### 2.5 HTTP 状态码（本篇会用到）

| 状态码 | 含义 | 何时返回 |
|--------|------|---------|
| `200 OK` | 成功 | 路径存在且正常处理 |
| `404 Not Found` | 未找到 | 路径不在路由表里 |
| `405 Method Not Allowed` | 方法不允许 | 暂只支持 GET，却来了 POST |

---

## 第三部分：完整代码 + 逐行讲解

### 3.1 完整代码

用下面代码写入 **`Step1to6/Step2.cpp`**（或在 `Step1to6/Step1.cpp` 基础上修改）：

```cpp
#include <iostream>
#include <cstring>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

// ---------- 数据结构：存放解析出的请求信息 ----------
struct HttpRequest {
    string method;
    string path;
    string version;
};

// ---------- 函数1：解析请求行 ----------
bool parseRequestLine(const string& raw, HttpRequest& req) {
    // 1. 找到第一行末尾（优先 \r\n，兼容只有 \n 的情况）
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == string::npos) {
        lineEnd = raw.find('\n');
    }
    if (lineEnd == string::npos) {
        return false;  // 连一行完整请求行都没有
    }

    // 2. 取出第一行，例如 "GET /about HTTP/1.1"
    string line = raw.substr(0, lineEnd);

    // 3. 按空格切分：method path version
    istringstream iss(line);
    if (!(iss >> req.method >> req.path >> req.version)) {
        return false;  // 格式不对（词不够三个）
    }
    return true;
}

// ---------- 函数2：把正文包装成完整 HTTP 响应 ----------
string buildHttpResponse(int statusCode,
                         const string& statusText,
                         const string& contentType,
                         const string& body) {
    string response =
        "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;
    return response;
}

// ---------- 函数3：路由——根据 path 决定返回什么 ----------
string handleRequest(const HttpRequest& req) {
    // 只支持 GET（浏览器地址栏访问就是 GET）
    if (req.method != "GET") {
        return buildHttpResponse(
            405, "Method Not Allowed",
            "text/plain; charset=utf-8",
            "暂只支持 GET 请求，收到的是: " + req.method
        );
    }

    // 路由表：path → 不同内容
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
            "这是关于页面。\nMyWebServer 是一个学习用的轻量级 Web 服务器。"
        );
    }

    if (req.path == "/hello") {
        return buildHttpResponse(
            200, "OK", "text/plain; charset=utf-8",
            "Hello! 路由生效了，你访问的是 /hello"
        );
    }

    // 没匹配到任何路由 → 404
    return buildHttpResponse(
        404, "Not Found", "text/plain; charset=utf-8",
        "404 - 页面不存在: " + req.path
    );
}

int main() {
    // ========== 以下 socket 部分与第一篇相同 ==========
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

    cout << "服务器已启动：http://127.0.0.1:8080\n";
    cout << "试试访问：/  /about  /hello  /任意不存在路径\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            cerr << "accept 失败\n";
            continue;
        }

        cout << "收到新连接\n";

        // ========== 读请求（同第一篇）==========
        char buffer[4096] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

        string response;

        if (n <= 0) {
            // 没读到有效数据，返回 400
            response = buildHttpResponse(
                400, "Bad Request", "text/plain; charset=utf-8",
                "无法读取请求"
            );
        } else {
            string rawRequest(buffer, n);
            cout << "收到请求（前 120 字符）：\n"
                 << rawRequest.substr(0, min(rawRequest.size(), size_t(120))) << "\n";

            // ========== 【本篇核心】解析 + 路由 ==========
            HttpRequest req;
            if (!parseRequestLine(rawRequest, req)) {
                response = buildHttpResponse(
                    400, "Bad Request", "text/plain; charset=utf-8",
                    "请求行格式错误，期望: GET /path HTTP/1.1"
                );
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

---

### 3.2 逐块讲解：三个新函数

#### 函数一：`parseRequestLine` —— 解析请求行

```cpp
bool parseRequestLine(const string& raw, HttpRequest& req)
```

| 部分 | 含义 |
|------|------|
| 返回 `bool` | 成功 `true`，失败 `false` |
| `const string& raw` | 原始 HTTP 请求全文（只读引用，不拷贝，省内存） |
| `HttpRequest& req` | 输出参数，解析结果写进这里 |

**第一步：找第一行在哪里结束**

```cpp
size_t lineEnd = raw.find("\r\n");
if (lineEnd == string::npos) {
    lineEnd = raw.find('\n');
}
```

HTTP 标准行尾是 `\r\n`，但有些工具只发 `\n`，所以两种都试。  
`string::npos` 表示"没找到"。

**第二步：截取第一行**

```cpp
string line = raw.substr(0, lineEnd);
```

例如得到：`GET /about HTTP/1.1`

**第三步：按空格切分**

```cpp
istringstream iss(line);
if (!(iss >> req.method >> req.path >> req.version)) {
    return false;
}
```

`istringstream` 像一条流水线，用 `>>` 按**空白**切：

```text
"GET /about HTTP/1.1"
  ↓      ↓        ↓
 GET  /about  HTTP/1.1
```

**为什么不用 `split`？** C++ 标准库没有简单的 split 函数，用 `istringstream` 是初学者最省事的写法。

---

#### 函数二：`buildHttpResponse` —— 自动拼响应

```cpp
string buildHttpResponse(int statusCode,
                         const string& statusText,
                         const string& contentType,
                         const string& body)
```

**本篇最大的改进之一：** `Content-Length` **不再手算**，用 `body.size()` 自动生成。

```cpp
"Content-Length: " + to_string(body.size()) + "\r\n"
```

| 好处 | 说明 |
|------|------|
| 避免第一篇的 bug | 改了正文忘记改 Content-Length |
| 支持中文 | `string::size()` 是**字节数**，UTF-8 中文正确 |
| 支持 HTML | 首页返回 `text/html`，其他页返回 `text/plain` |

调用示例：

```cpp
buildHttpResponse(200, "OK", "text/plain; charset=utf-8", "你好");
// 会自动算出 Content-Length: 6（UTF-8 下"你好"占 6 字节）
```

---

#### 函数三：`handleRequest` —— 路由

```cpp
string handleRequest(const HttpRequest& req)
```

逻辑就是**先判断 Method，再判断 Path**：

```text
method 不是 GET?  → 405
path 是 / ?       → 200 首页 HTML
path 是 /about ?  → 200 关于页
path 是 /hello ?  → 200 问候页
都不匹配 ?        → 404
```

**为什么 `/` 和 `/index.html` 都写？**  
有些浏览器或客户端会请求 `/index.html`，两个都指向首页更友好。

**为什么 404 正文里带上 `req.path`？**  
方便调试——你立刻知道用户访问了哪个不存在的路径。

---

### 3.3 逐块讲解：`main` 里变化的部分

#### 读请求后，先转成 `string`

```cpp
string rawRequest(buffer, n);
```

用 `buffer` 的前 `n` 字节构造 string，**不依赖 `\0` 结尾**，比直接当 C 字符串更安全。

#### 解析失败 vs 解析成功

```cpp
HttpRequest req;
if (!parseRequestLine(rawRequest, req)) {
    response = buildHttpResponse(400, "Bad Request", ...);
} else {
    cout << "解析结果: " << req.method << " " << req.path << " " << req.version << "\n";
    response = handleRequest(req);
}
```

| 情况 | 状态码 | 含义 |
|------|--------|------|
| 读不到数据 | 400 | Bad Request |
| 请求行格式错 | 400 | 不是 `METHOD PATH VERSION` |
| 路径不存在 | 404 | Not Found |
| 方法不是 GET | 405 | Method Not Allowed |
| 正常 | 200 | OK |

#### 统一用 `write` 发送

```cpp
write(client_fd, response.c_str(), response.size());
```

不管 200 还是 404，**响应格式是一样的**，只是状态码和正文不同。  
这就是把"拼 HTTP 头"抽成 `buildHttpResponse` 的好处。

---

### 3.4 与第一篇代码的对照表

| 位置 | 第一篇 | 第二篇 |
|------|--------|--------|
| 头文件 | 无 `<string>` `<sstream>` | 新增 |
| 读请求后 | 直接写死 Hello World | `parseRequestLine` → `handleRequest` |
| Content-Length | 手动写数字 | `body.size()` 自动算 |
| Content-Type | 固定 plain | 首页 `html`，其他 `plain` |
| 错误处理 | 几乎没有 | 400 / 404 / 405 |

---

## 编译、运行与测试

### 4.1 编译

在**项目根目录**执行：

```bash
g++ -std=c++17 -Wall -o server Step1to6/Step2.cpp
```

### 4.2 运行

仍在项目根目录：

```bash
./server
```

### 4.3 浏览器测试（推荐）

| 访问地址 | 预期页面 |
|---------|---------|
| `http://127.0.0.1:8080/` | 带链接的 HTML 首页 |
| `http://127.0.0.1:8080/about` | 纯文本"关于页面" |
| `http://127.0.0.1:8080/hello` | 纯文本问候语 |
| `http://127.0.0.1:8080/not-exist` | `404 - 页面不存在: /not-exist` |

终端应打印类似：

```text
收到新连接
收到请求（前 120 字符）：
GET /about HTTP/1.1
Host: 127.0.0.1:8080
...
解析结果: GET /about HTTP/1.1
已响应并关闭连接
```

### 4.4 curl 测试

```bash
# 首页
curl -i http://127.0.0.1:8080/

# 关于页
curl -i http://127.0.0.1:8080/about

# 404
curl -i http://127.0.0.1:8080/not-exist

# 模拟 POST（应返回 405）
curl -i -X POST http://127.0.0.1:8080/hello
```

`-i` 会显示响应头，方便确认状态码是 `200`、`404` 还是 `405`。

### 4.5 你应该看到的成功现象

| 测试 | 响应头第一行 | 正文特征 |
|------|-------------|---------|
| `/` | `HTTP/1.1 200 OK` | HTML，含超链接 |
| `/about` | `HTTP/1.1 200 OK` | 纯文本关于页 |
| `/not-exist` | `HTTP/1.1 404 Not Found` | 含路径名 |
| POST `/hello` | `HTTP/1.1 405 Method Not Allowed` | 提示只支持 GET |

---

## 常见问题排查

### Q1：首页乱码

**检查：** HTML 里是否有 `<meta charset="utf-8">`，响应头是否是 `text/html; charset=utf-8`。

### Q2：访问 `/about` 却显示首页内容

**原因：** 路径判断写错，或 `parseRequestLine` 没解析出正确的 path。

**调试：** 看终端 `解析结果:` 那一行，path 是否为 `/about`。

### Q3：404 页面在浏览器里"不好看"

正常。本篇 404 是纯文本，第三篇可以改成 HTML 404 页或返回静态文件。

### Q4：刷新一次，终端打印两条请求

浏览器可能额外请求 `/favicon.ico`（网站图标）。  
你会看到第二条 `解析结果: GET /favicon.ico HTTP/1.1`，然后 404——**这是正常现象**。  
可选：在 `handleRequest` 里专门处理 `/favicon.ico` 返回空 204。

### Q5：POST 表单提交不行？

本篇**故意只支持 GET**。表单 POST 留到以后扩展；现在用 curl `-X POST` 会收到 405。

### Q6：`parseRequestLine` 返回 false

常见原因：

- `read` 只读到半个请求（极少见，Demo 一般一次够）
- 请求不是 HTTP（用浏览器/curl 测不会遇到）

---

## 本篇小结与下一篇预告

### 你现在已经会了什么

- [x] 认识 HTTP 请求的**请求行**结构（Method、Path、Version）
- [x] 用 `find` / `substr` / `istringstream` **解析**请求行
- [x] 用 `if-else` 做**路由**，不同 URL 不同响应
- [x] 用 `buildHttpResponse` **自动计算** Content-Length
- [x] 返回正确的 **404** 和 **405** 状态码

### 和后续篇章的关系

```text
第一篇（socket + 固定 Hello World）
    │
    ▼
本篇（解析请求行 + 路由）
    │
    ├─→ 第三篇：读取磁盘上的 HTML/图片（静态文件）
    │
    ├─→ 第四篇：I/O 多路复用（epoll）
    │
    └─→ 第五篇：线程池 / 并发处理
```

### 建议你自己做的三个小实验

1. **加一条路由** `/time`：返回当前时间字符串——练手改 `handleRequest`。
2. **把 404 改成 HTML 页面**：`<h1>404</h1><p>找不到页面</p>`，并改 `Content-Type`。
3. **打印完整 path**：访问 `http://127.0.0.1:8080/hello?user=Tom`，观察 path 是 `/hello` 还是带参数——理解 **Query String 尚未解析**（以后可扩展）。

---

> **下一步：** 当你能用不同 URL 看到不同页面后，进入 [第三篇：返回静态文件](./03-返回静态文件指南.md)，让服务器从磁盘读取真实的 HTML、CSS、图片，而不是把页面内容写死在代码里。
