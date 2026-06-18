# 浏览器 URL 到页面显示流程详解

> **适用代码：** 分文件 Step3 静态文件版（`src/main.cpp`、`HttpParser`、`HttpHandler`、`HttpResponse`、`ServerConfig`）  
> **示例 URL：** `http://127.0.0.1:8080/` 与 `http://127.0.0.1:8080/about.html`  
> **模型：** 阻塞 `accept` → 读一次 → 处理一次 → 写一次 → 关闭连接

---

## 目录

1. [预备：服务器已在跑](#预备服务器已在跑)
2. [场景一：访问首页 `/`](#场景一访问首页-http1270018080)
3. [场景二：点击关于页 `/about.html`](#场景二点击关于页进入-http1270018080abouthtml)
4. [函数调用总览](#函数调用总览)
5. [路径映射表](#路径映射表)

---

## 预备：服务器已在跑

先在项目根目录执行 `make` 和 `./server`，进入 `main()` 后发生：

| 步骤 | 位置 | 做什么 |
|------|------|--------|
| 1 | `main()` | `socket()` 创建监听套接字 `server_fd` |
| 2 | `main()` | `setsockopt(..., SO_REUSEADDR, ...)` |
| 3 | `main()` | `bind(..., 8080)` 绑定端口 |
| 4 | `main()` | `listen(server_fd, 10)` 开始监听 |
| 5 | `main()` | 打印 `g_cfg.web_root`（`ServerConfig.cpp` 里 `g_cfg.web_root` 默认 `"www"`） |
| 6 | `main()` | 进入 `while(true)`，在 `accept()` 处**阻塞等待**浏览器连接 |

此时程序停在这里，等浏览器连上来。

---

## 场景一：访问首页 `http://127.0.0.1:8080/`

### A. 浏览器侧（还没进 C++ 业务函数）

1. 解析 URL：主机 `127.0.0.1`，端口 `8080`，路径 `/`。
2. 向 `127.0.0.1:8080` 建立 **TCP 连接**（三次握手）。
3. 发送 HTTP 请求报文，第一行大致是：

```http
GET / HTTP/1.1
Host: 127.0.0.1:8080
...
```

（后面还有请求头；当前代码**只解析第一行**，其余头不处理。）

---

### B. 服务器侧：`main()` 收到首页请求

```text
accept() → read() → parseRequestLine() → handleRequest() → write() → close()
```

逐步对应函数：

#### ① `accept(server_fd, ...)`（`main.cpp`）

- 从已完成握手的连接队列取出一个连接，得到 `client_fd`。
- 终端打印：`收到新连接`。

#### ② `read(client_fd, buffer, ...)`（`main.cpp`）

- 把浏览器发来的字节读进 `buffer`。
- 首页这次 `n > 0`，进入 `else` 分支。

#### ③ 构造 `rawRequest`（`main.cpp`）

```cpp
string rawRequest(buffer, n);
```

#### ④ `parseRequestLine(rawRequest, req)`（`HttpParser.cpp`）

- 找第一行结束符 `\r\n`（没有则找 `\n`）。
- 用 `istringstream` 解析出：
  - `req.method` = `"GET"`
  - `req.path` = `"/"`
  - `req.version` = `"HTTP/1.1"`
- 返回 `true`。
- `main()` 打印：`解析结果: GET /`。

#### ⑤ `pathToFile(req.path)`（`HttpResponse.cpp`，`main` 中仅用于打印）

- `urlPath == "/"` → 返回 `"www/index.html"`（映射到首页文件，不是 `"www/"`）。

#### ⑥ `handleRequest(req)`（`HttpHandler.cpp`）——核心业务

| 子步骤 | 函数 | 本次 `/` 的结果 |
|--------|------|----------------|
| 6.1 | `req.method != "GET"` 判断 | 是 GET，跳过 405 |
| 6.2 | `isPathSafe(req.path)` | `"/"` 无 `..`，返回 `true` |
| 6.3 | `pathToFile(req.path)` | 得到 `"www/index.html"` |
| 6.4 | `readFile(filePath, body)` | 打开磁盘 `www/index.html`，内容读入 `body` |
| 6.5 | `getContentType(filePath)` | 后缀 `.html` → `"text/html; chatset=utf-8"` |
| 6.6 | `buildHttpResponse(200, "OK", contentType, body)` | 拼完整 HTTP 响应字符串 |

#### ⑦ `buildHttpResponse(...)`（`HttpResponse.cpp`）

拼出类似：

```http
HTTP/1.1 200 OK
Content-Type: text/html; chatset=utf-8
Content_Length: ...
Connection: close

<!DOCTYPE html>...（index.html 全文）
```

#### ⑧ `write(client_fd, response.c_str(), response.size())`（`main.cpp`）

- 把响应写回浏览器。

#### ⑨ `close(client_fd)`（`main.cpp`）

- 关闭这条连接；打印 `已响应并关闭连接`。
- `main()` 回到 `while` 顶部，再次 `accept()` 等下一个连接。

---

### C. 浏览器收到首页 HTML 后（第二次请求：CSS）

`www/index.html` 中有：

```html
<link rel="stylesheet" href="/css/style.css">
```

浏览器解析 HTML 后，会**再发起一条新连接**（同样走上面整套流程），只是路径不同：

| 函数 | 这次 `/css/style.css` 的行为 |
|------|------------------------------|
| `parseRequestLine` | `req.path` = `"/css/style.css"` |
| `pathToFile` | `"/"` 不匹配 → `g_cfg.web_root + urlPath` → `"www/css/style.css"` |
| `readFile` | 读 CSS 文件 |
| `getContentType` | 后缀 `.css` → `"text/css; charset=utf-8"` |
| `buildHttpResponse` | 200 + CSS 正文 |

所以**完整显示带样式的首页**，通常是：**至少 2 次** `accept → ... → close`（HTML 一次，CSS 一次）。

---

### D. 浏览器最终呈现

1. 收到 `index.html`，解析出标题、段落、链接列表。
2. 收到 `style.css`，应用样式。
3. 页面上出现链接：**「关于页 about.html」**，对应 `href="/about.html"`（`www/index.html`）。

---

## 场景二：点击关于页，进入 `http://127.0.0.1:8080/about.html`

点击链接时，浏览器**不会**调用 C++ 里的函数；它根据 `href="/about.html"` 自动发新请求。服务器端再走一遍同样骨架，只是 `req.path` 变了。

### A. 浏览器侧

1. 导航到 `http://127.0.0.1:8080/about.html`（地址栏更新）。
2. 新建 TCP 连接（代码里 `Connection: close`，所以基本是**新连接**）。
3. 发送：

```http
GET /about.html HTTP/1.1
Host: 127.0.0.1:8080
...
```

---

### B. 服务器侧（函数调用链与首页相同）

```text
main(): accept()
     → read()
     → parseRequestLine(rawRequest, req)    // path = "/about.html"
     → handleRequest(req)
     → write()
     → close()
```

**`parseRequestLine` 结果：**

- `req.method` = `"GET"`
- `req.path` = `"/about.html"`
- `req.version` = `"HTTP/1.1"`

**`handleRequest` 内部：**

| 函数 | 对 `/about.html` 的行为 |
|------|-------------------------|
| `isPathSafe("/about.html")` | 无 `..`，通过 |
| `pathToFile("/about.html")` | 不等于 `/` 或 `/index.html` → 返回 `"www" + "/about.html"` = **`"www/about.html"`** |
| `readFile("www/about.html", body)` | 读 `about.html` 全文 |
| `getContentType("www/about.html")` | `.html` → `text/html; ...` |
| `buildHttpResponse(200, "OK", ...)` | 返回 about 页 HTML |

**`write` + `close`：** 浏览器收到 about 页内容。

---

### C. 关于页加载后的第二次请求（CSS）

`www/about.html` 中同样有：

```html
<link rel="stylesheet" href="/css/style.css">
```

浏览器会再请求 `/css/style.css`，流程与场景一里 CSS 请求**完全相同**（`pathToFile` → `www/css/style.css` → `readFile` → `getContentType` → `buildHttpResponse`）。

---

### D. 浏览器显示关于页

- 标题：**关于 MyWebServer**
- 正文说明从 `www/` 读文件
- 底部有 `<a href="/">返回首页</a>`，点它会再走一遍 `GET /` 的流程（`pathToFile` 映射到 `www/index.html`）。

---

## 函数调用总览

```text
main()
 ├── accept() / read() / write() / close()     ← 系统调用，网络 I/O
 ├── parseRequestLine()          [HttpParser.cpp]
 └── handleRequest()             [HttpHandler.cpp]
      ├── isPathSafe()           [HttpResponse.cpp]
      ├── pathToFile()           [HttpResponse.cpp]  ← 用 g_cfg.web_root
      ├── readFile()             [HttpResponse.cpp]
      ├── getContentType()       [HttpResponse.cpp]
      └── buildHttpResponse()    [HttpResponse.cpp]
```

---

## 路径映射表

`pathToFile()` 的规则：

| 浏览器 URL 路径 | `pathToFile` 结果 | 磁盘文件 |
|----------------|-------------------|----------|
| `/` | `www/index.html` | 首页 |
| `/index.html` | `www/index.html` | 首页 |
| `/about.html` | `www/about.html` | 关于页 |
| `/css/style.css` | `www/css/style.css` | 样式表 |

---

## 补充说明

### 超链接 vs 按钮

页面上是 **超链接** `<a href="/about.html">`，不是表单按钮。对服务器来说没有区别：**都是浏览器自发 `GET` 新 URL**。服务器永远从 `accept()` 开始，不会记住「用户是从首页点过来的」——每次请求独立，靠 URL 路径决定读哪个文件。

### 404 分支

若 `readFile` 失败（例如文件不存在），`handleRequest` 会走 404 分支，调用 `buildHttpResponse(404, "Not Found", ...)`，不会走成功的 200 分支。

### 400 分支

若 `read` 失败或 `parseRequestLine` 返回 `false`，`main()` 直接调用 `buildHttpResponse(400, "Bad Request", ...)`，不会进入 `handleRequest`。

---

> **相关文档：** [03-返回静态文件指南](./03-返回静态文件指南.md) · [分文件 Step3](./分文件编写指南/03-Step3-HttpResponse静态文件.md)
