# 分文件 Step1 —— main 最小 Web 服务器（详细教程）

> **写给谁看：** 已完成单文件 [01-最小可行Web服务器指南](../01-最小可行Web服务器指南.md) 的学习者，准备在 `src/` 目录搭建**可交付的多文件工程**。  
> **本篇目标：** 创建 `src/main.cpp` 与最简 `Makefile`，跑通阻塞 `accept` + 固定 `Hello World!` 响应。  
> **不做的事：** 不拆 HttpParser、不解析请求、不读静态文件 —— 这些从 [Step2](./02-Step2-HttpTypes与HttpParser.md) 开始。  
> **前置文档：** [00-总览与工程结构](./00-总览与工程结构.md) · [01-最小可行Web服务器指南](../01-最小可行Web服务器指南.md)  
> **单文件对照：** [Step1to6/Step1.cpp](../../Step1to6/Step1.cpp)

---

## 目录

1. [第一部分：架构位置、为什么要拆、生活类比](#第一部分架构位置为什么要拆生活类比)
2. [第二部分：本步文件树（前后对比）](#第二部分本步文件树前后对比)
3. [第三部分：每个文件的完整代码](#第三部分每个文件的完整代码)
4. [第四部分：逐文件 / 逐块讲解](#第四部分逐文件--逐块讲解)
5. [Makefile 完整说明](#makefile-完整说明)
6. [编译、运行与测试](#编译运行与测试)
7. [常见问题排查](#常见问题排查)
8. [与单文件 Step1.cpp 对照表](#与单文件-step1cpp-对照表)
9. [本篇小结与下一篇链接](#本篇小结与下一篇链接)

---

## 第一部分：架构位置、为什么要拆、生活类比

### 1.1 本篇在分文件系列中的位置

你已经用单文件 `Step1.cpp` 跑通了最小服务器。分文件系列不是「重写逻辑」，而是把同一份能跑的代码**搬进工程目录**，为后续模块拆分打地基。

```text
分文件系列进度
────────────────────────────────────────────────────────
Step1（本篇）  src/main.cpp + Makefile
    │          全部逻辑还在 main 里，和单文件 Step1 等价
    ▼
Step2          拆出 HttpTypes / HttpParser / HttpResponse / HttpHandler
    ▼
Step3          读 www/ 静态文件，引入 ServerConfig
    ▼
Step4～11      epoll、线程池、MySQL……（见 00-总览）
```

本篇结束时，你的仓库里会同时存在两套「第一步」：

| 位置 | 用途 |
|------|------|
| `Step1to6/Step1.cpp` | 学习历程归档，答辩时展示「从单文件走来」 |
| `src/main.cpp` | **日常开发入口**，后续 Step 都在此工程上叠加 |

### 1.2 为什么 Step1 就要分目录，而不是继续改 Step1.cpp？

| 理由 | 说明 |
|------|------|
| **交付形态** | 真实项目、简历项目通常是 `src/` + `Makefile`，不是 600 行单文件 |
| **增量演进** | Step2 要加 6 个文件；若仍挤在 `Step1.cpp` 里，每步 diff 巨大、难以 review |
| **命名分离** | 源码放 `src/`（目录），可执行文件叫 `server`（根目录文件）——避免与单文件阶段 `g++ -o server` 产物同名冲突 |
| **编译约定** | 根目录 `make` → 产出 `./server`，与后续读 `www/`、写 `logs/` 的路径约定一致 |

**生活类比：** 单文件版像「所有菜谱抄在一张餐巾纸上」——能做饭，但不好扩展。分文件 Step1 像「先买一本空白菜谱本，第一页写上第一道菜的完整步骤」——内容没变，但本子已经准备好了。

### 1.3 浏览器访问时，本篇程序在做什么？

和单文件 Step1 完全一样，扮演「餐厅窗口」：

| 生活场景 | 对应到程序 |
|---------|-----------|
| 餐厅挂出门牌「8080 号窗口」 | `bind` 到端口 8080 |
| 服务员站在窗口等客人 | `listen` + `accept` 阻塞等待 |
| 客人说一段话（HTTP 请求） | `read` 读进 buffer，**打印但不解析** |
| 不管客人要什么，都递同一张纸条 | 固定返回 `Hello World!` |
| 送走这位客人，继续等下一位 | `close(client_fd)`，回到 `accept` |

### 1.4 本篇程序整体流程

```text
  启动 ./server（项目根目录）
     │
     ▼
  socket() 创建监听套接字
     │
     ▼
  setsockopt(SO_REUSEADDR)  开发时避免 bind 失败
     │
     ▼
  bind(8080) + listen(10)
     │
     ▼
  ┌─→ accept() 阻塞等待浏览器
  │      │
  │      ▼
  │   read() 读 HTTP 请求（打印，不解析）
  │      │
  │      ▼
  │   拼固定 HTTP 响应（Hello World!）
  │      │
  │      ▼
  │   write() 发出响应
  │      │
  │      ▼
  │   close(client_fd)
  │      │
  └──────┘  循环
```

### 1.5 本篇需要的 Linux API 一览

| 头文件 | 提供的函数 / 类型 | 生活类比 |
|--------|------------------|---------|
| `<sys/socket.h>` | `socket`, `bind`, `listen`, `accept`, `setsockopt` | 电话系统：装机、绑号、待机、接听 |
| `<netinet/in.h>` | `sockaddr_in`, `htons`, `INADDR_ANY` | 门牌与地址格式 |
| `<arpa/inet.h>` | 网络字节序工具 | 地址书写规范 |
| `<unistd.h>` | `read`, `write`, `close` | 听、说、挂电话 |
| `<iostream>` | `cout`, `cerr` | 在终端记工作日志 |

> **环境：** 请在 **Linux 或 WSL2** 下编译。Windows 原生 MSVC 的 socket API 不同，本系列按 Linux 讲解。

---

## 第二部分：本步文件树（前后对比）

### 2.1 开始本篇之前

假设你已有学习用单文件（仓库默认即有）：

```text
MyWebServer/
├── Step1to6/
│   └── Step1.cpp          ← 单文件第一步（保留，不删）
├── www/                   ← Step3 才用；本篇可忽略
└── docs/
```

此时还**没有** `src/` 目录，也**没有** 根目录 `Makefile`。

### 2.2 完成本篇之后

```text
MyWebServer/
├── src/                ← 【本篇新建目录】
│   └── main.cpp           ← 【本篇新建】阻塞模型，全部逻辑在此
├── Makefile               ← 【本篇新建】最简：只编译 main.cpp
├── server                 ← make 后生成（可执行文件，非目录）
├── Step1to6/
│   └── Step1.cpp          ← 保留对照
├── www/                   ← 尚未使用
└── docs/分文件编写指南/
    └── 01-Step1-main最小服务器.md  ← 本文
```

### 2.3 本步新增 / 修改清单

| 操作 | 路径 | 说明 |
|------|------|------|
| **新建** | `src/main.cpp` | 从 `Step1.cpp`「搬家」，逻辑等价 |
| **新建** | `Makefile` | `g++` 编译出根目录可执行文件 `server` |
| **不建** | `HttpParser.h` 等 | Step2 才出现 |
| **不删** | `Step1to6/Step1.cpp` | 永远保留作对照 |

---

## 第三部分：每个文件的完整代码

> 以下代码均可**整段复制**，无需手写 `// ...` 省略部分。

### 3.1 `src/main.cpp`（完整）

在仓库根目录执行：

```bash
# 若根目录已有单文件阶段编译出的 server 可执行文件，先移走或删除
rm -f server          # 或：mv server server.bak
mkdir -p src
```

然后创建 `src/main.cpp`，写入：

```cpp
#include <iostream>
#include <cstring>
#include <algorithm>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

int main() {
    // ---------- 1. 创建 socket ----------
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "socket 创建失败\n";
        return 1;
    }

    // ---------- 2. 允许端口快速重用（开发强烈建议）----------
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ---------- 3. 填写地址并绑定端口 ----------
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "bind 失败（端口可能被占用）\n";
        close(server_fd);
        return 1;
    }

    // ---------- 4. 开始监听 ----------
    if (listen(server_fd, 10) < 0) {
        cerr << "listen 失败\n";
        close(server_fd);
        return 1;
    }

    cout << "服务器已启动：http://127.0.0.1:8080\n";
    cout << "在浏览器打开上述地址，或用 curl 测试\n";

    // ---------- 5. 主循环：不断接受连接 ----------
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
        if (n > 0) {
            cout << "收到请求（完整报文，共 " << n << " 字节）：\n";
            cout << string(buffer, n) << "\n";
        }

        const char* body = "Hello World!";
        string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 12\r\n"
            "Connection: close\r\n"
            "\r\n"
            + string(body);

        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "已响应并关闭连接\n";
    }

    close(server_fd);
    return 0;
}
```

### 3.2 `Makefile`（完整）

在**项目根目录**（与 `src/`、`www/` 同级）创建 `Makefile`：

```makefile
# MyWebServer 分文件工程 —— Step1 最简 Makefile
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Isrc

.PHONY: all clean

all: server

server: src/main.cpp
	$(CXX) $(CXXFLAGS) -o server src/main.cpp

clean:
	rm -f server
```

---

## 第四部分：逐文件 / 逐块讲解

### 4.1 `src/main.cpp` 头文件区

| 头文件 | 作用 | 本篇是否必需 |
|--------|------|-------------|
| `<iostream>` | `cout` / `cerr` 打日志 | 是 |
| `<cstring>` | C 字符串工具（习惯上保留；本篇主要靠 `std::string`） | 建议 |
| `<algorithm>` | `std::min`（若只打印前 N 字符时用；本篇打印全文可不用，保留无害） | 可选 |
| `<string>` | `std::string` 拼 HTTP 响应 | 是 |
| `<unistd.h>` | `read` / `write` / `close` | 是 |
| `<sys/socket.h>` | socket 全家桶 | 是 |
| `<netinet/in.h>` | `sockaddr_in`、`htons` | 是 |
| `<arpa/inet.h>` | 与网络地址相关的辅助 | 是 |

**分文件启示：** Step1 所有 `#include` 都在 `main.cpp`。Step2 起，`HttpParser.cpp` 会引入 `<sstream>`，`HttpResponse.cpp` 不再需要 socket 头——**谁用谁 include**，避免一个 cpp 拖入全部依赖。

### 4.2 创建 socket 与错误处理

```cpp
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
```

| 参数 | 含义 |
|------|------|
| `AF_INET` | IPv4 |
| `SOCK_STREAM` | TCP（可靠字节流） |
| `0` | 默认协议 |

返回值是**文件描述符** `server_fd`；失败为 `-1`。  
`server_fd` 是「餐厅窗口」本身，整个进程生命周期内通常只创建一次。

### 4.3 `SO_REUSEADDR`

```cpp
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

Ctrl+C 结束程序后，端口可能处于 `TIME_WAIT`，立刻重启会 `bind 失败`。  
此选项让内核允许**快速重新绑定**同一端口。开发阶段几乎必加。

> 单文件 `Step1.cpp` 未加此项；分文件版加上，并在后文对照表注明。

### 4.4 `sockaddr_in` 与 `bind`

```cpp
sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = INADDR_ANY;
addr.sin_port = htons(8080);
```

| 字段 | 值 | 含义 |
|------|-----|------|
| `sin_family` | `AF_INET` | IPv4 地址族 |
| `sin_addr.s_addr` | `INADDR_ANY` | 监听本机所有网卡（`127.0.0.1` 与局域网 IP 均可连） |
| `sin_port` | `htons(8080)` | 端口 8080，主机字节序转网络字节序 |

`bind` 把 `server_fd` 和「本机:8080」绑定。失败最常见原因：**端口已被占用**。

### 4.5 `listen` 与 `accept`

```cpp
listen(server_fd, 10);
```

第二个参数 `10` 是**已完成三次握手、等待 `accept` 取走的连接队列**上限。  
单文件 Step1 与分文件版均用 `10`；高并发场景后面会换 epoll。

```cpp
int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
```

- **阻塞：** 没有浏览器连接时，进程停在这里。
- **返回 `client_fd`：** 与**这一位**客人通信的专用 fd。
- `server_fd` 继续负责接待下一位，不要 `close(server_fd)`。

### 4.6 读取请求（不解析）

```cpp
char buffer[4096] = {0};
ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
```

浏览器典型请求形如：

```http
GET / HTTP/1.1
Host: 127.0.0.1:8080
User-Agent: curl/8.5.0
Accept: */*

```

本篇策略：**读进来、打印全文、不分析** `GET` 还是 `POST`。  
Step2 会把「找第一行、拆 method/path」挪到 `HttpParser.cpp`。

### 4.7 构造固定 HTTP 响应

```cpp
const char* body = "Hello World!";
string response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 12\r\n"
    "Connection: close\r\n"
    "\r\n"
    + string(body);
```

| 响应部分 | 内容 | 注意 |
|---------|------|------|
| 状态行 | `HTTP/1.1 200 OK` | 协议写 `HTTP` 全大写（规范） |
| `Content-Type` | `text/plain; charset=utf-8` | 纯文本 |
| `Content-Length` | `12` | 必须等于 `body` 字节数：`Hello World!` 共 12 字符 |
| `Connection` | `close` | 发完就关连接，简单模型够用 |
| 空行 | `\r\n` | **头与正文的分隔**，缺了浏览器解析失败 |
| 正文 | `Hello World!` | 浏览器页面显示的内容 |

```cpp
write(client_fd, response.c_str(), response.size());
close(client_fd);
```

`write` 一次发出整段响应；`close(client_fd)` 结束本次会话，循环回到 `accept`。

### 4.8 为什么 Step1 不拆文件？

| 若现在拆 | 问题 |
|---------|------|
| 把 `buildHttpResponse` 抽到 `HttpResponse.cpp` | 只有一处调用，拆完反而要多维护两个文件 |
| 把 socket 循环抽到 `Server.cpp` | Step4 还要引入 epoll，过早抽象会推倒重来 |

**结论：** Step1 保持「一个 cpp 说清楚 socket 流程」；Step2 当**第二处**需要「拼响应」和「解析请求」时再拆，边界最自然。

---

## Makefile 完整说明

### 5.1 各变量含义

| 变量 / 目标 | 含义 |
|------------|------|
| `CXX = g++` | C++ 编译器 |
| `CXXFLAGS = -std=c++17 -Wall -Isrc` | C++17、开警告、头文件搜索路径包含 `src/` |
| `all` | 默认目标，生成 `server` 可执行文件 |
| `server` | 依赖 `src/main.cpp`，链接为根目录 `./server` |
| `clean` | 删除 `./server` |

### 5.2 为什么在根目录生成 `server`？

```text
MyWebServer/          ← 你在这里 make && ./server
├── src/main.cpp
├── www/              ← Step3 相对路径 "www" 以根目录为 cwd
└── server            ← 可执行文件
```

Step3 起 `web_root = "www"` 是相对**进程工作目录**的。习惯在根目录运行 `./server`，与单文件教程一致。

### 5.3 Step2 时 Makefile 如何变？

Step2 会增加 `HttpParser.cpp` 等，把：

```makefile
server: src/main.cpp
	$(CXX) $(CXXFLAGS) -o server src/main.cpp
```

改成多文件 `SRCS` 列表（见 Step2 文档）。Step1 故意保持最简，让你看清「第一次只有 main」。

---

## 编译、运行与测试

### 6.1 环境要求

- Linux 或 WSL2
- g++ 支持 C++17：`sudo apt install g++ build-essential`（Debian/Ubuntu）

### 6.2 编译

```bash
cd /path/to/MyWebServer
make
```

**预期终端输出：**（无 error，可能有 `-Wall` 警告则检查代码）

```text
g++ -std=c++17 -Wall -Isrc -o server src/main.cpp
```

成功后根目录出现可执行文件 `server`。

### 6.3 运行

```bash
./server
```

**预期输出：**

```text
服务器已启动：http://127.0.0.1:8080
在浏览器打开上述地址，或用 curl 测试
```

进程保持前台运行，光标阻塞等待连接。

### 6.4 浏览器测试

1. 打开 Chrome / Firefox
2. 地址栏：`http://127.0.0.1:8080`
3. 页面应只显示：**Hello World!**

**服务器终端应追加类似：**

```text
收到新连接
收到请求（完整报文，共 78 字节）：
GET / HTTP/1.1
Host: 127.0.0.1:8080
...
已响应并关闭连接
```

### 6.5 curl 测试

另开终端：

```bash
curl -v http://127.0.0.1:8080
```

**预期片段：**

```text
< HTTP/1.1 200 OK
< Content-Type: text/plain; charset=utf-8
< Content-Length: 12
< Connection: close
<
Hello World!
```

### 6.6 验收清单

| 检查项 | 通过标准 |
|--------|---------|
| `make` | 无编译错误 |
| 浏览器 | 显示 `Hello World!` |
| curl | 状态 200，正文 12 字节 |
| 终端 | 每次访问打印完整请求报文 |
| 多次访问 | 每次都能响应（循环 `accept` 正常） |
| `make clean` | 删除 `./server` 成功 |

### 6.7 与单文件 Step1 并行对照（可选）

想确认「搬家」没改行为，可同时编译单文件版：

```bash
g++ -std=c++17 -Wall -o server_single Step1to6/Step1.cpp
./server_single
```

两者浏览器表现应一致（均为固定 Hello World!）；分文件版多了 `SO_REUSEADDR`，重启更省事。

---

## 常见问题排查

### Q1：`make: g++: Command not found`

**原因：** 未安装编译器。  
**办法：** `sudo apt update && sudo apt install g++ make`

### Q2：`bind 失败（端口可能被占用）`

**原因：** 8080 被占或 `TIME_WAIT`。  
**办法：**

1. 查占用：`ss -tlnp | grep 8080` 或 `lsof -i:8080`
2. 结束旧进程：`kill <pid>`
3. 确认已加 `SO_REUSEADDR`
4. 临时改端口：代码中 `htons(8080)` → `htons(8081)`，浏览器访问 `:8081`

### Q3：浏览器空白或一直加载

**检查：**

- 响应行是否 `HTTP/1.1`（不是 `Http/1.1`）
- 头字段是否以 `\r\n` 结尾
- 头与正文之间是否有**空行**（`\r\n\r\n`）
- `Content-Length: 12` 是否与 `Hello World!` 一致

### Q4：`./server: No such file or directory`

**原因：** 未 `make` 或不在项目根目录。  
**办法：** `cd` 到含 `Makefile` 的目录，执行 `make`。

### Q5：Windows PowerShell 里直接运行报错

本系列使用 Linux socket API。请在 **WSL** 内执行 `make` 与 `./server`，不要在纯 Windows 原生环境编译本篇代码。

### Q6：访问后终端没有「收到新连接」

**检查：**

- 服务器是否在跑、是否绑对端口
- 防火墙是否拦截
- URL 是否写错（`http://` 不是 `https://`）

### Q7：能不能把 `main.cpp` 放在根目录？

可以，但**不推荐**。`src/` 目录为 Step2～11 预留了 `HttpParser.cpp` 等位置；根目录只放 `Makefile`、`www/`、可执行文件 `server`，结构更清晰。

### Q8：`mkdir src` 报错或无法创建目录？

**原因：** 根目录可能已存在名为 `server` 的**可执行文件**（单文件阶段 `g++ -o server` 生成）。文件与文件夹不能同名，因此不能用 `server/` 作源码目录。  
**办法：** 按 [00-总览](./00-总览与工程结构.md) 的**方法 B**，源码一律放 `src/`；先 `rm -f server` 再 `mkdir -p src`。

---

## 与单文件 Step1.cpp 对照表

| 项目 | 单文件 `Step1to6/Step1.cpp` | 分文件 `src/main.cpp`（本篇） |
|------|---------------------------|--------------------------------|
| 入口文件 | `Step1to6/Step1.cpp` | `src/main.cpp` |
| 编译方式 | `g++ -o server Step1to6/Step1.cpp` | 根目录 `make` |
| 可执行文件位置 | 自行 `-o` 指定 | 根目录 `./server` |
| `SO_REUSEADDR` | 无 | **有**（开发友好） |
| 正文内容 | `Hello World!` | 相同 |
| `Content-Length` | 12 | 12 |
| `listen`  backlog | 10 | 10 |
| 请求处理 | 读入并打印全文 | 相同 |
| 解析 HTTP | 无 | 无 |
| 变量名笔误 | `reponse`（拼写错误） | 已改为 `response` |
| 逻辑所在 | 全部在单文件 | 全部仍在 `main.cpp`（尚未拆模块） |

**迁移本质：** 把 `Step1.cpp` 的 `main` 函数**原样搬进** `src/main.cpp`，修正笔误、补上 `SO_REUSEADDR`，并加上 `Makefile`。socket 流程一行不多、一行不少。

---

## 本篇小结与下一篇链接

### 你现在已经会了什么

- [x] 在 `src/` 下建立交付工程的第一步
- [x] 用根目录 `Makefile` 编译出 `./server`
- [x] 阻塞 `accept` 模型处理浏览器连接
- [x] 固定 HTTP 响应让页面显示 `Hello World!`
- [x] 理解「单文件归档」与「多文件交付」并存

### 与后续篇章的关系

```text
本篇（分文件 Step1：只有 main.cpp）
    │
    ├─→ Step2：拆 HttpTypes / HttpParser / HttpResponse / HttpHandler
    │
    ├─→ Step3：读 www/ 静态文件 + ServerConfig
    │
    └─→ Step4～11：epoll、线程池、日志、MySQL……
```

### 建议你自己做的两个小实验

1. 把正文改成 `你好`，同时把 `Content-Length` 改成 `6`（UTF-8 下每个汉字 3 字节）——体会**长度必须精确**。
2. 故意删掉响应头后的空行 `\r\n`，刷新浏览器——体会 **HTTP 格式** 的重要性。

---

> **下一步：** 确认 `make && ./server` 稳定显示 `Hello World!` 后，进入 [02-Step2-HttpTypes与HttpParser](./02-Step2-HttpTypes与HttpParser.md)，把「解析请求行」「拼响应」「简单路由」从 `main` 拆到独立模块。
