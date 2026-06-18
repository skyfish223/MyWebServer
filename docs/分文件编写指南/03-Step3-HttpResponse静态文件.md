# 分文件 Step3 —— HttpResponse 读 www 静态文件（详细教程）

> **写给谁看：** 已完成 [分文件 Step2](./02-Step2-HttpTypes与HttpParser.md) 的学习者——能解析请求行、按 URL 做内存路由。  
> **本篇目标：** 根据 URL 从磁盘读取 `www/` 下的 HTML、CSS、图片等静态文件；引入 `ServerConfig` 管理 `web_root`。  
> **不做的事：** 不用 epoll、不用多线程、不支持 POST 上传 —— 这些留到 Step4 及以后。  
> **前置文档：** [00-总览与工程结构](./00-总览与工程结构.md) · [02-Step2-HttpTypes与HttpParser](./02-Step2-HttpTypes与HttpParser.md) · [03-返回静态文件指南](../03-返回静态文件指南.md)  
> **单文件对照：** [Step1to6/Step3.cpp](../../Step1to6/Step3.cpp)

---

## 目录

1. [第一部分：架构位置、为什么要拆、生活类比](#第一部分架构位置为什么要拆生活类比)
2. [第二部分：本步文件树（前后对比）](#第二部分本步文件树前后对比)
3. [第三部分：准备 www 目录与示例文件](#第三部分准备-www-目录与示例文件)
4. [第四部分：每个文件的完整代码](#第四部分每个文件的完整代码)
5. [第五部分：逐文件 / 逐块讲解](#第五部分逐文件--逐块讲解)
6. [Makefile 完整说明](#makefile-完整说明)
7. [编译、运行与测试](#编译运行与测试)
8. [常见问题排查](#常见问题排查)
9. [与单文件 Step3.cpp 对照表](#与单文件-step3cpp-对照表)
10. [本篇小结与下一篇链接](#本篇小结与下一篇链接)

---

## 第一部分：架构位置、为什么要拆、生活类比

### 1.1 本篇在架构中的位置

Step2 的 HTML 写在 `HttpHandler.cpp` 的字符串里。Step3 升级为：**URL 路径映射到磁盘文件**，页面改版只需改 `www/*.html`，重启服务器即可（无需重编译 C++）。

```text
  HttpRequest.path
        │
        ▼
  HttpHandler::handleRequest
        │
        ├─ isPathSafe(path) ── 含 .. ──► 403
        │
        ▼
  HttpResponse::pathToFile(path)  ── 拼 www/...
        │
        ▼
  HttpResponse::readFile ── 失败 ──► 404
        │
        ▼
  HttpResponse::getContentType
        │
        ▼
  HttpResponse::buildHttpResponse(200, ..., body)
```

**配置从哪来？** `ServerConfig::g_cfg.web_root` 默认 `"www"`，替代单文件里的 `const string WEB_ROOT`。

### 1.2 Step2 vs Step3：差在哪？

| 对比项 | Step2（分文件） | Step3（本篇） |
|--------|----------------|--------------|
| 页面内容 | C++ 内嵌字符串 | 磁盘 `www/` |
| 改首页 | 改 `HttpHandler.cpp` + `make` | 改 `www/index.html` + 重启 |
| CSS / 图片 | 难以维护 | 浏览器自动请求 `/css/style.css` 等 |
| 路由 | 多个 `if (path == ...)` | **统一规则**：`www + path` |
| 安全配置 | 无 | `isPathSafe` 防 `..` 越界 |
| 新增模块 | — | `ServerConfig` |

### 1.3 生活类比

| Step2 | Step3 |
|-------|-------|
| 菜单背在服务员脑子里 | 菜单是**仓库里的真实菜谱本**（`www/`） |
| 加一道菜要重新培训服务员 | 加一页 `about.html` 就行 |
| 配菜（CSS）没法单独管理 | 菜谱本里有「样式页」抽屉（`css/`） |

### 1.4 URL 如何对应磁盘路径？

约定网站根目录 `web_root = "www"`（相对**进程工作目录**，即项目根）：

```text
浏览器 URL path              磁盘文件（本篇规则）
──────────────────────────────────────────────────
/                             →  www/index.html
/index.html                   →  www/index.html
/about.html                   →  www/about.html
/css/style.css                →  www/css/style.css
```

核心公式：

```text
pathToFile(urlPath) =
    若 urlPath 是 / 或 /index.html → web_root + "/index.html"
    否则                           → web_root + urlPath
```

### 1.5 为什么必须设置 Content-Type？

| Content-Type | 浏览器行为 |
|--------------|-----------|
| `text/html` | 渲染为网页 |
| `text/css` | 当作样式表 |
| `image/png` | 显示图片 |
| 设错（如 HTML 当纯文本） | 页面乱码或样式失效 |

### 1.6 一次访问首页，其实有多次请求

浏览器打开 `http://127.0.0.1:8080/` 时：

```text
1) GET /              → 服务器读 www/index.html
2) GET /css/style.css → 服务器读 www/css/style.css（HTML 里 <link> 触发）
```

服务器对**每个 TCP 连接**仍是一次 `read` → 响应 → `close`（阻塞模型未变）。这就是为什么静态站点必须能正确提供 CSS。

### 1.7 本篇整体流程

```text
  （Step2 已有：socket → accept → read → parseRequestLine）
     │
     ▼
  【新增】isPathSafe(req.path)
     │
     ▼
  【新增】pathToFile(req.path) → 磁盘路径
     │
     ▼
  【新增】readFile → string body
     │
     ▼
  【新增】getContentType(文件路径)
     │
     ▼
  buildHttpResponse → write → close
```

---

## 第二部分：本步文件树（前后对比）

### 2.1 Step2 结束时

```text
MyWebServer/
├── src/
│   ├── main.cpp
│   ├── HttpTypes.h
│   ├── HttpParser.h / HttpParser.cpp
│   ├── HttpResponse.h / HttpResponse.cpp    ← 仅有 buildHttpResponse
│   └── HttpHandler.h / HttpHandler.cpp      ← 内嵌 HTML 字符串路由
├── Makefile
└── www/                                     ← 可能已有示例，但未使用
```

### 2.2 Step3 结束时

```text
MyWebServer/
├── src/
│   ├── main.cpp                             ← 【小改】日志、buffer 8192
│   ├── ServerConfig.h                       ← 【新建】
│   ├── ServerConfig.cpp                     ← 【新建】g_cfg 定义
│   ├── HttpTypes.h                          ← 不变
│   ├── HttpParser.h / HttpParser.cpp        ← 不变
│   ├── HttpResponse.h / HttpResponse.cpp    ← 【扩展】静态文件函数
│   └── HttpHandler.h / HttpHandler.cpp      ← 【改写】走磁盘
├── Makefile                                 ← 【更新】加 ServerConfig.cpp
├── www/                                     ← 【必备】网站根目录
│   ├── index.html
│   ├── about.html
│   └── css/
│       └── style.css
└── Step1to6/Step3.cpp                       ← 单文件对照（保留）
```

### 2.3 本步操作清单

| 操作 | 文件 |
|------|------|
| 新建 | `ServerConfig.h`, `ServerConfig.cpp` |
| 扩展 | `HttpResponse.h`, `HttpResponse.cpp` |
| 改写 | `HttpHandler.cpp`, `main.cpp`（小改） |
| 更新 | `Makefile` |
| 准备 | `www/` 下示例 HTML / CSS |

---

## 第三部分：准备 www 目录与示例文件

在**项目根目录**创建 `www/` 及子目录。若仓库已自带 `www/`，请核对内容与下表一致或兼容。

### 3.1 `www/index.html`（完整）

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>MyWebServer 首页</title>
  <link rel="stylesheet" href="/css/style.css">
</head>
<body>
  <h1>欢迎访问 MyWebServer</h1>
  <p>这是从磁盘读取的 <code>www/index.html</code>，不是写在 C++ 里的字符串。</p>
  <ul>
    <li><a href="/about.html">关于页 about.html</a></li>
    <li><a href="/not-exist.html">故意 404</a></li>
  </ul>
</body>
</html>
```

### 3.2 `www/about.html`（完整）

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>关于</title>
  <link rel="stylesheet" href="/css/style.css">
</head>
<body>
  <h1>关于 MyWebServer</h1>
  <p>第三篇：服务器根据 URL 路径，读取 <code>www/</code> 目录下的真实文件。</p>
  <p><a href="/">返回首页</a></p>
</body>
</html>
```

### 3.3 `www/css/style.css`（完整）

```css
body {
  font-family: sans-serif;
  max-width: 640px;
  margin: 2rem auto;
  padding: 0 1rem;
  line-height: 1.6;
}

h1 {
  color: #2563eb;
}

code {
  background: #f1f5f9;
  padding: 0.1em 0.4em;
  border-radius: 4px;
}
```

### 3.4 目录结构确认

```bash
cd /path/to/MyWebServer
find www -type f
```

**预期：**

```text
www/index.html
www/about.html
www/css/style.css
```

---

## 第四部分：每个文件的完整代码

### 4.1 `src/ServerConfig.h`（新建）

```cpp
#pragma once

#include <string>

struct ServerConfig {
    std::string web_root = "www";
};

extern ServerConfig g_cfg;
```

### 4.2 `src/ServerConfig.cpp`（新建）

```cpp
#include "ServerConfig.h"

ServerConfig g_cfg;
```

### 4.3 `src/HttpResponse.h`（完整替换）

```cpp
#pragma once

#include <string>

std::string buildHttpResponse(int statusCode,
                              const std::string& statusText,
                              const std::string& contentType,
                              const std::string& body);

std::string pathToFile(const std::string& urlPath);
bool isPathSafe(const std::string& urlPath);
std::string getContentType(const std::string& filePath);
bool readFile(const std::string& filePath, std::string& out);
```

### 4.4 `src/HttpResponse.cpp`（完整替换）

```cpp
#include "HttpResponse.h"
#include "ServerConfig.h"

#include <fstream>
#include <iterator>
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

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html") {
        return g_cfg.web_root + "/index.html";
    }
    return g_cfg.web_root + urlPath;
}

bool isPathSafe(const string& urlPath) {
    if (urlPath.find("..") != string::npos) {
        return false;
    }
    return true;
}

string getContentType(const string& filePath) {
    if (filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".html") {
        return "text/html; charset=utf-8";
    }
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".css") {
        return "text/css; charset=utf-8";
    }
    if (filePath.size() >= 3 && filePath.substr(filePath.size() - 3) == ".js") {
        return "application/javascript; charset=utf-8";
    }
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".png") {
        return "image/png";
    }
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".jpg") {
        return "image/jpeg";
    }
    if (filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".jpeg") {
        return "image/jpeg";
    }
    return "application/octet-stream";
}

bool readFile(const string& filePath, string& out) {
    ifstream ifs(filePath, ios::binary);
    if (!ifs) {
        return false;
    }
    out.assign(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
    return true;
}
```

### 4.5 `src/HttpHandler.h`（不变，列出便于对照）

```cpp
#pragma once

#include "HttpTypes.h"
#include <string>

std::string handleRequest(const HttpRequest& req);
```

### 4.6 `src/HttpHandler.cpp`（完整替换）

```cpp
#include "HttpHandler.h"
#include "HttpResponse.h"

using namespace std;

string handleRequest(const HttpRequest& req) {
    if (req.method != "GET") {
        return buildHttpResponse(
            405, "Method Not Allowed",
            "text/plain; charset=utf-8",
            "暂只支持 GET，收到:" + req.method);
    }

    if (!isPathSafe(req.path)) {
        return buildHttpResponse(
            403, "Forbidden",
            "text/plain; charset=utf-8",
            "403 - 非法路径");
    }

    string filePath = pathToFile(req.path);
    string body;
    if (!readFile(filePath, body)) {
        return buildHttpResponse(
            404, "Not Found",
            "text/plain; charset=utf-8",
            "404 - 文件不存在: " + req.path);
    }

    string contentType = getContentType(filePath);
    return buildHttpResponse(200, "OK", contentType, body);
}
```

### 4.7 `src/HttpTypes.h`（不变）

```cpp
#pragma once

#include <string>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
};
```

### 4.8 `src/HttpParser.h` / `HttpParser.cpp`（不变，与 Step2 相同）

**HttpParser.h**

```cpp
#pragma once

#include "HttpTypes.h"
#include <string>

bool parseRequestLine(const std::string& raw, HttpRequest& req);
```

**HttpParser.cpp**

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

### 4.9 `src/main.cpp`（完整）

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
#include "ServerConfig.h"

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
        cerr << "bind 失败\n";
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        cerr << "listen 失败\n";
        close(server_fd);
        return 1;
    }

    cout << "静态文件服务器已启动：http://127.0.0.1:8080\n";
    cout << "网站根目录: " << g_cfg.web_root << "/\n";
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

        char buffer[8192] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

        string response;
        if (n <= 0) {
            response = buildHttpResponse(
                400, "Bad Request",
                "text/plain; charset=utf-8",
                "无法读取请求");
        } else {
            string rawRequest(buffer, n);
            HttpRequest req;
            if (!parseRequestLine(rawRequest, req)) {
                response = buildHttpResponse(
                    400, "Bad Request",
                    "text/plain; charset=utf-8",
                    "请求行格式错误");
            } else {
                cout << "解析结果: " << req.method << " " << req.path << "\n";
                cout << "读取文件: " << pathToFile(req.path) << "\n";
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

### 4.10 `Makefile`（完整）

```makefile
# MyWebServer 分文件工程 —— Step3
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Isrc

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
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

## 第五部分：逐文件 / 逐块讲解

### 5.1 `ServerConfig` —— 配置从硬编码到全局对象

#### 单文件写法（Step3.cpp）

```cpp
const string WEB_ROOT = "www";
```

#### 分文件写法（本篇）

| 位置 | 内容 |
|------|------|
| `ServerConfig.h` | `struct ServerConfig { string web_root = "www"; };` + `extern ServerConfig g_cfg;` |
| `ServerConfig.cpp` | `ServerConfig g_cfg;`  **定义**全局实例 |

**为何用 `extern`？**

- 头文件被多个 cpp include 时，若在 `.h` 里写 `ServerConfig g_cfg;` 会导致 **multiple definition**
- 正确模式：**声明**在 `.h`，**定义**在唯一一个 `.cpp`

**Step11 预告：** 同一文件会扩展端口、线程数、数据库参数；`g_cfg` 名不变，避免全项目改名。

### 5.2 `pathToFile` —— URL 到磁盘路径

```cpp
if (urlPath == "/" || urlPath == "/index.html")
    return g_cfg.web_root + "/index.html";
return g_cfg.web_root + urlPath;
```

| 请求 path | 返回路径（默认 web_root） |
|-----------|-------------------------|
| `/` | `www/index.html` |
| `/about.html` | `www/about.html` |
| `/css/style.css` | `www/css/style.css` |

**注意：** `web_root + urlPath` 在 path 已带前导 `/` 时得到 `www/about.html`（正确）。不要写成 `web_root + "/" + urlPath` 否则变成 `www//about.html`（多数系统能容忍，但不规范）。

### 5.3 `isPathSafe` —— 最简单的目录穿越防护

```cpp
if (urlPath.find("..") != string::npos) return false;
```

| 请求 | 结果 |
|------|------|
| `/about.html` | 安全 |
| `/../etc/passwd` | 拒绝 → 403 |
| `/foo/../../secret` | 拒绝 → 403 |

**局限：** 生产环境还需规范化路径、禁止符号链接越界等；本篇够用教学。

**检查对象：** 应对 **URL path**（`req.path`）检查，而不是拼接后的磁盘路径——与单文件 Step3 一致。

### 5.4 `readFile` —— 二进制安全读取

```cpp
ifstream ifs(filePath, ios::binary);
out.assign(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
```

| 要点 | 说明 |
|------|------|
| `ios::binary` | 图片等二进制不被换行转换破坏 |
| `istreambuf_iterator` | 一次读入整个文件到 `std::string` |
| 失败返回 `false` | Handler 转 404 |

**内存提示：** 大文件全部进内存；高并发项目会用 mmap 或流式发送——Step8 分次写会涉及。

### 5.5 `getContentType` —— 按后缀选 MIME

| 后缀 | Content-Type |
|------|--------------|
| `.html` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js` | `application/javascript; charset=utf-8` |
| `.png` | `image/png` |
| `.jpg` / `.jpeg` | `image/jpeg` |
| 其它 | `application/octet-stream` |

实现方式：检查 `filePath` 末尾子串（与单文件 Step3 相同）。更正规可用 `std::filesystem::path::extension()`，本篇保持简单。

### 5.6 `HttpHandler` 瘦身对比

| Step2 Handler | Step3 Handler |
|---------------|---------------|
| 多个 `if (path == "/about")` | **无 path 分支** |
| 返回内嵌 HTML 字符串 | `readFile` + `getContentType` |
| 404 写死在路由表外 | 文件不存在统一 404 |

**统一规则的好处：** 新增 `www/help.html` 无需改 C++，访问 `/help.html` 即可。

### 5.7 `main.cpp` 相对 Step2 的小改动

| 改动 | 原因 |
|------|------|
| `#include "ServerConfig.h"` | 启动时打印 `g_cfg.web_root` |
| `buffer[8192]` | 与单文件 Step3 一致，略大于 Step2 |
| 日志 `pathToFile(req.path)` | 调试时看见实际读哪个文件 |
| 启动文案「静态文件服务器」 | 与 Step2 区分 |

Parser / accept 循环结构**不变**——体现「换 Handler 内部实现，main 保持稳定」。

### 5.8 模块依赖（Step3 更新）

```text
ServerConfig.cpp  →  g_cfg
        ▲
        │
HttpResponse.cpp  →  pathToFile 使用 g_cfg.web_root
        ▲
        │
HttpHandler.cpp
        ▲
        │
main.cpp
```

---

## Makefile 完整说明

### 6.1 相对 Step2 的变化

```makefile
SRCS = src/main.cpp \
       src/ServerConfig.cpp \    ← 新增这一行
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp
```

漏加 `ServerConfig.cpp` → 链接错误 `undefined reference to g_cfg`。

### 6.2 编译与运行目录

必须在**项目根目录**（含 `www/` 文件夹）执行：

```bash
make && ./server
```

若在其他目录启动，`web_root` 相对路径会对错，导致 404。

---

## 编译、运行与测试

### 7.1 编译

```bash
cd /path/to/MyWebServer
make clean
make
```

**预期：** 无 error，生成 `./server`。

### 7.2 启动

```bash
./server
```

**预期输出：**

```text
静态文件服务器已启动：http://127.0.0.1:8080
网站根目录: www/
在浏览器打开上述地址，或用 curl 测试
```

### 7.3 测试用例表

| # | 命令 | 预期 |
|---|------|------|
| 1 | `curl -s http://127.0.0.1:8080/` | HTML 含「从磁盘读取」 |
| 2 | `curl -s http://127.0.0.1:8080/about.html` | 关于页 HTML |
| 3 | `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/not-exist.html` | `404` |
| 4 | `curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:8080/../etc/passwd"` | `403` |
| 5 | `curl -s -I http://127.0.0.1:8080/css/style.css` | `Content-Type: text/css` |
| 6 | 浏览器打开首页 | 蓝色标题、排版正常（CSS 加载成功） |

### 7.4 测试 1 详情

```bash
curl -s http://127.0.0.1:8080/ | head -5
```

**预期片段：**

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>MyWebServer 首页</title>
```

**服务器终端：**

```text
收到新连接
解析结果: GET /
读取文件: www/index.html
已响应并关闭连接
```

### 7.5 测试 CSS 二次请求

访问首页后，终端应**再出现**一条：

```text
解析结果: GET /css/style.css
读取文件: www/css/style.css
```

说明浏览器自动拉了样式表，静态服务链路完整。

### 7.6 修改 www 不重编译（核心体验）

```bash
# 另开终端，不改 C++
echo '<h1>临时改版</h1>' > www/index.html
# 重启 ./server 后刷新浏览器
```

应看到新标题——这就是静态文件的意义。（生产环境可用热更新 / 反代，本篇不展开。）

### 7.7 验收清单

| 检查项 | 通过标准 |
|--------|---------|
| `make` | 5 个 cpp 链接成功 |
| 首页 | 来自 `www/index.html` |
| CSS | 样式生效 |
| 404 | 不存在文件返回 404 |
| 403 | 含 `..` 的 path 被拒绝 |
| 配置 | 启动日志打印 `www/` |

---

## 常见问题排查

### Q1：所有页面都 404，日志显示 `读取文件: www/index.html`

**原因：** 工作目录不对，进程找不到 `www/`。  
**办法：** 只在项目根目录运行 `./server`；或 `ls www/index.html` 确认文件存在。

### Q2：HTML 有内容但样式全乱

**原因：** CSS 请求 404 或 `Content-Type` 错误。  
**办法：**

1. `curl -I http://127.0.0.1:8080/css/style.css` 看状态与类型
2. 确认 `www/css/style.css` 存在
3. 确认 `getContentType` 对 `.css` 返回 `text/css`

### Q3：`undefined reference to g_cfg`

**原因：** `Makefile` 未编译 `ServerConfig.cpp`。  
**办法：** 对照 4.10 节补全 `SRCS`。

### Q4：中文页面乱码

**检查：** HTML 内 `<meta charset="utf-8">`；响应头 `text/html; charset=utf-8`。

### Q5：`403 - 非法路径` 误伤正常 URL

**检查：** 是否 path 里真的含有子串 `..`；正常 path 不应触发。

### Q6：图片无法显示

**检查：** 是否已把 `.png` / `.jpg` 加入 `getContentType`；`readFile` 是否 `ios::binary`。

### Q7：单文件 Step3 能访问，分文件版不行

逐项对照 [第九节对照表](#与单文件-step3cpp-对照表)；重点查 `g_cfg.web_root` 默认值、`pathToFile` 逻辑、cwd。

### Q8：修改 `HttpHandler` 后仍返回旧 HTML 字符串

**原因：** 未保存文件或未 `make` 重装。  
**办法：** `make clean && make`，确认 `HttpHandler.cpp` 已无大段 `if (path == "/about")` 字符串。

---

## 与单文件 Step3.cpp 对照表

| 项目 | 单文件 `Step3.cpp` | 分文件 Step3（本篇） |
|------|-------------------|---------------------|
| 网站根目录 | `const WEB_ROOT = "www"` | `g_cfg.web_root`（`ServerConfig`） |
| `pathToFile` | 第 46～53 行 | `HttpResponse.cpp` |
| `isPathSafe` | 第 56～60 行 | `HttpResponse.cpp` |
| `getContentType` | 第 63～78 行 | `HttpResponse.cpp`（一致） |
| `readFile` | 第 81～90 行 | `HttpResponse.cpp` |
| `buildHttpResponse` | 第 34～44 行（有笔误） | `HttpResponse.cpp`（已修正，见下） |
| `handleRequest` | 第 93～120 行 | `HttpHandler.cpp` |
| `parseRequestLine` | 第 22～32 行 | `HttpParser.cpp`（Step2 已有） |
| `main` | 第 122～190 行 | `main.cpp` |
| `buffer` 大小 | 8192 | 8192 |
| `SO_REUSEADDR` | 有 | 有 |

### 相对单文件有意修正的差异

| 单文件笔误 / 不规范 | 分文件处理 |
|--------------------|-----------|
| `"Http/1.1" + to_string(...)` 缺少空格 | `"HTTP/1.1 " + to_string(...)` |
| 无 `Connection: close` | 已添加 |
| `Content-type` 大小写混用 | 统一 `Content-Type` |

逻辑与验收行为与单文件 Step3 **对齐**（修正 HTTP 格式后浏览器更稳定）。

---

## 本篇小结与下一篇链接

### 你现在已经会了什么

- [x] 用 `ServerConfig::g_cfg.web_root` 配置网站根目录
- [x] 在 `HttpResponse` 中实现路径映射、安全检查、读文件、MIME
- [x] `HttpHandler` 从「内存路由」升级为「统一静态文件服务」
- [x] 浏览器一次访问触发 HTML + CSS 多次请求的正确响应
- [x] 改 `www/` 即可改站，无需重编译 C++

### 与后续篇章的关系

```text
本篇（静态文件 + ServerConfig 雏形）
    │
    ├─→ Step4：epoll + EpollHelper，accept 不再阻塞整进程
    │
    ├─→ Step5：ThreadPool，耗时逻辑丢进工作线程
    │
    ├─→ Step6：HttpParser 状态机，支持 POST /echo
    │
    └─→ Step11：ServerConfig 扩展为完整配置 + 命令行
```

### 建议实验

1. 在 `www/` 新建 `demo.txt`，访问 `/demo.txt`——验证「零 C++ 改动加文件」。
2. 访问 `/../etc/passwd`，确认 403——体会最小安全校验。
3. 用浏览器开发者工具 Network 面板，看 `/` 与 `/css/style.css` 两次请求的状态码与类型。

---

> **下一步：** 进入 [04-Step4-epoll与EpollHelper](./04-Step4-epoll与EpollHelper.md)，用 epoll 替换阻塞 `accept`，为高并发打骨架。
