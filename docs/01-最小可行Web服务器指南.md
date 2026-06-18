# 最小可行 Web 服务器 —— 零基础学习指南（第一篇）

> **写给谁看：** 计算机专业学生，学过 C++、计算机网络、操作系统，但已经两年没碰，几乎从零开始。  
> **本篇目标：** 跑起来一个能响应浏览器请求的极简服务器。  
> **不做的事：** 不解析 HTTP、不用 epoll、不用多线程、不返回静态文件 —— 这些留到后面的篇章。

---

## 目录

1. [第一部分：先搞懂"我们在做什么"](#第一部分先搞懂我们在做什么)
2. [第二部分：需要哪些"工具"，为什么要用](#第二部分需要哪些工具为什么要用)
3. [第三部分：完整代码 + 逐行讲解](#第三部分完整代码--逐行讲解)
4. [编译、运行与测试](#编译运行与测试)
5. [常见问题排查](#常见问题排查)
6. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

> **复习用：** 函数忘了先翻 [01-第一步函数速查表.md](./01-第一步函数速查表.md)（只记「有啥、干啥」）；细节再回本篇或 [01-第一步代码深度答疑.md](./01-第一步代码深度答疑.md)。

---

## 第一部分：先搞懂"我们在做什么"

### 1.1 浏览器访问网站，背后发生了什么？

想象你去一家餐厅吃饭：

| 生活场景 | 对应到 Web 服务器 |
|---------|------------------|
| 餐厅有一个**门牌号**（比如"8080号窗口"） | 服务器监听一个**端口号**（比如 8080） |
| 你走到窗口，**敲门说"我要一份菜单"** | 浏览器发送一条 **HTTP 请求** |
| 服务员**听到你的话**，去厨房拿菜单给你 | 服务器**读取请求**，准备**响应** |
| 服务员把菜单**递给你** | 服务器把 **HTTP 响应** 发回浏览器 |
| 你**离开窗口** | 连接**关闭** |

我们的极简 Demo，就是扮演"餐厅窗口"的角色：开门营业、等客人来、听到一句话（不细究内容）、固定回一句"Hello World"。

### 1.2 什么是 Socket（套接字）？

**Socket** 可以理解为：**网络通信的"电话听筒"**。

- 你拿起听筒（创建 socket）
- 绑定你的电话号码（bind 到端口）
- 开始等电话（listen）
- 有人打来，你接听（accept）
- 听对方说话（read），你回话（write）
- 挂电话（close）

Linux 下，这些操作都通过一组 C 语言风格的系统调用完成，C++ 可以直接调用它们。

### 1.3 什么是 HTTP 响应？（本篇只需知道"长什么样"）

浏览器能看懂的内容，必须按 **HTTP 协议格式** 来写。最简的响应大概长这样：

```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 11
Connection: close

Hello World
```

逐段理解：

- **第一行**：`HTTP/1.1 200 OK` —— 协议版本 + 状态码（200 表示成功）+ 简短说明
- **中间几行**：响应头（告诉浏览器"这是纯文本、多长、发完就关连接"）
- **空行**：头和正文之间的分隔（**必须有**，浏览器靠它判断正文从哪开始）
- **最后一行**：正文内容 `Hello World`

本篇我们**不解析**浏览器发来的请求，只**固定**返回上面这段文字。

### 1.4 本篇程序的整体流程

用一张图串起来：

```text
  启动程序
     │
     ▼
  创建 socket（电话听筒）
     │
     ▼
  绑定端口 8080（门牌号）
     │
     ▼
  开始监听（等电话）
     │
     ▼
  ┌─→ 接受一个连接（接听）
  │      │
  │      ▼
  │   读取请求（听一句，不分析）
  │      │
  │      ▼
  │   发送 "Hello World" 响应
  │      │
  │      ▼
  │   关闭这次连接
  │      │
  └──────┘  （循环：继续等下一个连接）
```

---

## 第二部分：需要哪些"工具"，为什么要用

### 2.1 头文件：告诉编译器"我要用哪些函数"

| 头文件 | 提供什么 | 为什么需要 |
|--------|---------|-----------|
| `<iostream>` | `std::cout` 等 | 在终端打印日志，方便调试 |
| `<cstring>` | `memset` 等 | 清零结构体，避免脏数据 |
| `<unistd.h>` | `read`, `write`, `close` | 读写字节、关闭文件描述符 |
| `<sys/socket.h>` | `socket`, `bind`, `listen`, `accept` | 创建和管理 socket |
| `<netinet/in.h>` | `sockaddr_in`, `htons` 等 | 描述 IPv4 地址和端口 |
| `<arpa/inet.h>` | 地址转换相关 | 网络字节序工具（本篇用到 `htons`） |

> **注意：** 这些头文件是 **Linux / WSL** 下的。Windows 原生环境 API 不同；本系列按 **Linux** 讲解，建议在 WSL 或 Linux 虚拟机里编译运行。

### 2.2 核心函数一览

| 函数 | 作用 | 生活类比 |
|------|------|---------|
| `socket()` | 创建一个 socket | 买一部电话 |
| `bind()` | 把 socket 和端口绑定 | 给电话分配号码 8080 |
| `listen()` | 开始监听 | 打开"等待来电"模式 |
| `accept()` | 接受一个客户端连接 | 接起一通电话 |
| `read()` | 从连接读数据 | 听对方说话 |
| `write()` | 向连接写数据 | 你回话 |
| `close()` | 关闭连接或 socket | 挂电话 |

### 2.3 两个重要概念（够用版）

#### （1）文件描述符（fd）

Linux 里，**一切皆文件**。socket 连接成功后，会给你一个**整数编号**（比如 3、4、5），叫**文件描述符**。后面的 `read` / `write` / `close` 都用这个编号操作。

- `server_fd`：监听用的 socket（餐厅窗口本身）
- `client_fd`：某一次客人连接（某一通电话）

#### （2）`sockaddr_in` 结构体

存 IPv4 地址和端口。我们只用**本地所有网卡** + **8080 端口**，即"本机任意 IP 的 8080 端口都能连进来"。

#### （3）`SO_REUSEADDR`（可选但强烈建议）

程序刚退出时，端口可能还被系统占用几秒。设置这个选项后，可以**立刻重新绑定同一端口**，开发时少踩坑。

---

## 第三部分：完整代码 + 逐行讲解

### 3.1 完整代码

把下面代码保存为 **`Step1to6/Step1.cpp`**（或对照仓库中已有文件修改）：

```cpp
#include <iostream>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    // ---------- 1. 创建 socket ----------
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket 创建失败\n";
        return 1;
    }

    // ---------- 2. 允许端口快速重用 ----------
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // ---------- 3. 填写地址信息并绑定端口 ----------
    sockaddr_in addr{};
    addr.sin_family = AF_INET;           // IPv4
    addr.sin_addr.s_addr = INADDR_ANY;   // 监听本机所有网卡
    addr.sin_port = htons(8080);         // 端口 8080

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind 失败（端口可能被占用）\n";
        close(server_fd);
        return 1;
    }

    // ---------- 4. 开始监听 ----------
    if (listen(server_fd, 128) < 0) {
        std::cerr << "listen 失败\n";
        close(server_fd);
        return 1;
    }

    std::cout << "服务器已启动：http://127.0.0.1:8080\n";
    std::cout << "在浏览器打开上述地址，或用 curl 测试\n";

    // ---------- 5. 主循环：不断接受连接 ----------
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        // 5.1 阻塞等待，直到有浏览器连进来
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "accept 失败\n";
            continue;  // 这次失败不退出，继续等下一个
        }

        std::cout << "收到新连接\n";

        // 5.2 读取 HTTP 请求（不解析，读到就行）
        char buffer[4096] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            std::cout << "收到请求（前 200 字符）：\n"
                      << std::string(buffer, std::min(n, (ssize_t)200)) << "\n";
        }

        // 5.3 构造并发送固定 HTTP 响应
        const char* body = "Hello World";
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            + std::string(body);

        write(client_fd, response.c_str(), response.size());

        // 5.4 关闭本次连接
        close(client_fd);
        std::cout << "已响应并关闭连接\n";
    }

    close(server_fd);  // 实际 while(true) 不会执行到这里
    return 0;
}
```

---

### 3.2 逐行讲解

#### 头文件区域

```cpp
#include <iostream>
```
引入标准输入输出流。后面用 `std::cout` 打印日志、`std::cerr` 打印错误。

```cpp
#include <cstring>
```
引入 C 风格字符串工具。后面可能用到 `memset`；同时 `<cstring>` 也提供 `strlen` 等（本篇主要用 C++ 的 `std::string`）。

```cpp
#include <unistd.h>
```
**Unix 标准**头文件。提供 `read`、`write`、`close` —— 读写 socket、关闭 fd 都靠它。

```cpp
#include <sys/socket.h>
```
**Socket 核心 API**：`socket`、`bind`、`listen`、`accept`、`setsockopt`。

```cpp
#include <netinet/in.h>
```
**IPv4 互联网地址**相关：`sockaddr_in` 结构体、`htons`、`INADDR_ANY` 等。

```cpp
#include <arpa/inet.h>
```
**地址转换**函数（如 `inet_ntoa`）。本篇主要间接用到其中的网络字节序工具；和 `<netinet/in.h>` 常一起出现。

---

#### 创建 socket

```cpp
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
```

| 参数 | 含义 |
|------|------|
| `AF_INET` | 使用 **IPv4** 地址族 |
| `SOCK_STREAM` | 使用 **TCP**（可靠、有序的字节流） |
| `0` | 自动选择 TCP 协议 |

返回值是文件描述符。失败返回 `-1`。

```cpp
if (server_fd < 0) {
    std::cerr << "socket 创建失败\n";
    return 1;
}
```
出错就打印信息并退出。`return 1` 表示程序异常结束（非 0 一般代表有错）。

---

#### 设置 SO_REUSEADDR

```cpp
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

- `setsockopt`：给 socket **设置选项**
- `SO_REUSEADDR`：允许**地址重用**

**为什么？** 你 Ctrl+C 停掉服务器后，系统可能认为端口"还在冷却"。不加这句，经常遇到 `bind 失败`。开发阶段几乎必加。

---

#### 填写地址并 bind

```cpp
sockaddr_in addr{};
```
定义一个 IPv4 地址结构体。`{}` 把它**零初始化**，避免未赋值字段里是随机垃圾。

```cpp
addr.sin_family = AF_INET;
```
说明这是 **IPv4** 地址。

```cpp
addr.sin_addr.s_addr = INADDR_ANY;
```
`INADDR_ANY` 表示：**监听本机所有网卡**。  
客人可以用 `127.0.0.1:8080`（本机）或局域网 IP 连进来（如果防火墙允许）。

```cpp
addr.sin_port = htons(8080);
```
端口号设为 **8080**。  
**为什么用 `htons`？** 网络传输时，多字节整数要用**大端字节序**（网络字节序）。`htons` = **h**ost **to** **n**etwork **s**hort，把本机 short 转成网络字节序。  
8080 是常见开发端口，大于 1024，普通用户权限就能绑定（不用 root）。

```cpp
if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
```
`bind`：把 `server_fd` 和"8080 端口"绑在一起。  
第二个参数类型是 `sockaddr*`，所以要把 `sockaddr_in*` **强转**成通用指针（经典 C API 写法）。

失败常见原因：**8080 已被别的程序占用**。

---

#### listen 开始监听

```cpp
if (listen(server_fd, 128) < 0) {
```

| 参数 | 含义 |
|------|------|
| `server_fd` | 要监听的 socket |
| `128` | **等待队列**最大长度 —— 同时有很多连接请求在"排队等 accept"时，最多排 128 个 |

调用成功后，内核会在 8080 端口上**等客户端发起 TCP 三次握手**。  
此时还**没有**真正建立客人连接，只是"营业中"的状态。

---

#### 主循环：accept

```cpp
while (true) {
```
服务器应该**一直运行**，处理完一个客人继续等下一个。

```cpp
sockaddr_in client_addr{};
socklen_t client_len = sizeof(client_addr);
```
`accept` 需要知道**是谁连进来的**，会把客户端 IP 和端口写进 `client_addr`。  
`client_len` 是入参/出参：告诉内核结构体有多大，返回时可能被改成实际长度。

```cpp
int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
```
**阻塞等待**：没有客人时，程序停在这里睡觉；有 TCP 连接完成时醒来，返回**新的 fd** `client_fd`，专门用于和**这一位客人**通信。  
`server_fd` 继续负责接待下一位。

```cpp
if (client_fd < 0) {
    std::cerr << "accept 失败\n";
    continue;
}
```
单次失败不必让整个服务器退出，`continue` 回到循环开头继续等。

---

#### 读取 HTTP 请求（不解析）

```cpp
char buffer[4096] = {0};
ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
```

- `read`：从 `client_fd` 读最多 4095 字节到 `buffer`（留 1 字节给 `\0`，方便当字符串打印）
- 返回值 `n`：实际读到的字节数；`0` 表示对端关闭；`-1` 表示出错

浏览器发来的请求通常长这样（看一眼即可）：

```http
GET / HTTP/1.1
Host: 127.0.0.1:8080
User-Agent: Mozilla/5.0 ...
Accept: text/html,...
```

**本篇策略：** 读进来、打印前面一段帮助学习，**不分析**请求行和 Header。  
以后篇章再学怎么解析 `GET /index.html` 等。

```cpp
if (n > 0) {
    std::cout << "收到请求（前 200 字符）：\n"
              << std::string(buffer, std::min(n, (ssize_t)200)) << "\n";
}
```
把读到的内容打印出来，方便你在终端**亲眼看到浏览器发了什么**。  
只打印前 200 字符，避免刷屏。文件顶部已 `#include <algorithm>` 以使用 `std::min`。

---

#### 发送固定 HTTP 响应

```cpp
const char* body = "Hello World";
```
正文内容。长度是 **11** 个字符（含中间空格，不含结尾 `\0`）。

```cpp
std::string response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    + std::string(body);
```

逐行说明：

| 行 | 作用 |
|----|------|
| `HTTP/1.1 200 OK\r\n` | 状态行。`\r\n` 是 HTTP 规定的行结束符（回车+换行） |
| `Content-Type: ...` | 告诉浏览器：正文是 UTF-8 纯文本 |
| `Content-Length: 11` | 正文正好 11 字节，浏览器知道读多少 |
| `Connection: close` | 响应发完后关闭连接（简单服务器常用） |
| 空行 `\r\n` | **头和正文的分隔**，缺了浏览器会解析失败 |
| `Hello World` | 正文 |

```cpp
write(client_fd, response.c_str(), response.size());
```
把整段响应**写入连接**，发给浏览器。  
`c_str()` 拿到 C 风格字符串指针；`size()` 是字节数（不是 `strlen`，因为 `std::string` 可能含 `\0`，本篇没有）。

---

#### 关闭连接

```cpp
close(client_fd);
```
这一通电话打完，挂断。`server_fd` 还在，继续 `accept` 下一位。

```cpp
close(server_fd);
```
理论上程序退出前关闭监听 socket。因为我们写了 `while (true)`，正常跑不会执行到；加上是良好习惯。

---

## 编译、运行与测试

### 4.1 环境要求

- **Linux** 或 **WSL2**（Windows 子系统 Linux）
- 安装 g++：`sudo apt install g++`（Debian/Ubuntu）

### 4.2 编译

在**项目根目录**（能看到 `Step1to6/` 和后续会用到的 `www/` 同级结构）执行：

```bash
g++ -std=c++17 -Wall -o server Step1to6/Step1.cpp
```

| 选项 | 含义 |
|------|------|
| `-std=c++17` | 使用 C++17 标准 |
| `-Wall` | 打开常见警告，帮助发现笔误 |
| `-o server` | 输出可执行文件 `server`（放在项目根，便于后续 Step 读 `www/`） |

> Step1～6 源码均在 [`Step1to6/`](../Step1to6/) 目录；本篇对应 `Step1.cpp`。

### 4.3 运行

仍在**项目根目录**执行：

```bash
./server
```

终端应显示：

```text
服务器已启动：http://127.0.0.1:8080
在浏览器打开上述地址，或用 curl 测试
```

### 4.4 用浏览器测试

1. 打开 Chrome / Firefox
2. 地址栏输入：`http://127.0.0.1:8080`
3. 页面应显示：**Hello World**

同时，运行 `./server` 的终端会打印收到的 HTTP 请求片段。

### 4.5 用 curl 测试（命令行）

另开一个终端：

```bash
curl -v http://127.0.0.1:8080
```

`-v` 会显示完整的请求和响应头，适合学习。

### 4.6 你应该看到的成功现象

| 位置 | 现象 |
|------|------|
| 浏览器 | 页面只有 `Hello World` |
| 服务器终端 | `收到新连接` → 请求内容 → `已响应并关闭连接` |
| curl | `HTTP/1.1 200 OK` 和正文 `Hello World` |

---

## 常见问题排查

### Q1：`bind 失败（端口可能被占用）`

**原因：** 8080 已被占用，或上次程序退出端口还在 TIME_WAIT。

**办法：**

1. 换端口：把代码里两处 `8080` 改成 `8081`
2. 查占用：`ss -tlnp | grep 8080` 或 `lsof -i:8080`
3. 确认已加 `SO_REUSEADDR`
4. 杀掉旧进程：`kill <pid>`

### Q2：浏览器一直转圈，没有内容

**检查：**

- 响应头是否用 `\r\n` 而不是只写 `\n`
- 头和正文之间是否有**空行**
- `Content-Length` 是否和正文长度一致（`Hello World` 是 11）

### Q3：`accept 失败`

可能监听 fd 已损坏或被关闭。看终端错误信息；开发阶段少见。

### Q4：Windows 下直接编译报错

本代码用的是 **Linux socket API**。请使用 **WSL**，或在 Linux 虚拟机 / 云服务器上编译。

### Q5：只能处理一个连接？

不会。我们在 `while (true)` 里**串行**处理：一个连接处理完再 `accept` 下一个。  
同时来很多连接时会排队 —— 够用做 Demo；后面篇章用 **多线程 / epoll** 改进。

---

## 本篇小结与下一篇预告

### 你现在已经会了什么

- [x] 用 `socket` → `bind` → `listen` 把服务器"开起来"
- [x] 用 `accept` 接待浏览器连接
- [x] 用 `read` 看到原始 HTTP 请求
- [x] 用固定格式的 HTTP 响应让浏览器显示 `Hello World`

### 和后续篇章的关系

```text
本篇（最小 Demo）
    │
    ├─→ 第二篇：解析 HTTP 请求（Method、Path、Header）
    │
    ├─→ 第三篇：根据路径返回不同内容 / 静态文件
    │
    ├─→ 第四篇：I/O 多路复用（epoll）
    │
    └─→ 第五篇：线程池 / 并发处理
```

### 建议你自己做的两个小实验

1. 把正文改成 `你好，世界`，同时修改 `Content-Length` —— 体会**头和正文必须一致**。
2. 故意删掉响应里的空行 `\r\n`，看浏览器表现 —— 体会 **HTTP 格式** 的重要性。

---

> **下一步：** 当你能在浏览器稳定看到 `Hello World` 后，就可以进入第二篇 —— **解析 HTTP 请求**，让服务器根据 URL 返回不同内容。

