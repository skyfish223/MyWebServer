# Step5 之后完善路线图 —— 对照 TinyWebServer 的详细步骤

> **写给谁看：** 已完成 [Step5 / 第五篇](./05-线程池指南.md)（[Step5.cpp](../Step5.cpp) 或 [00-Step5完整默写手册](./00-Step5完整默写手册.md)）的学习者。  
> **本文目标：** 说明 Step5 **完成之后**，MyWebServer 还应按什么顺序、每一步具体做什么、如何验收、与 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 哪块代码对照。  
> **不是一篇就能写完的代码教程：** 本文是**总路线图 + 每一步的设计说明**；你按 Step6、Step7… 分文件实现时，可再为每一步单独写「指南 / 默写手册」（风格同 01～05）。  
> **前置文档：** [01](./01-最小可行Web服务器指南.md) · [02](./02-解析HTTP请求指南.md) · [03](./03-返回静态文件指南.md) · [04](./04-epoll高并发指南.md) · [05](./05-线程池指南.md)

---

## 目录

1. [总览：Step5 之后要走多远](#一总览step5-之后要走多远)
2. [开工前：建议先做的整理](#二开工前建议先做的整理)
3. [Step6：HTTP 状态机（请求行 → 头 → 体）+ POST](#三step6http-状态机请求行--头--体--post)
4. [Step7：定时器 —— 踢掉非活跃连接](#四step7定时器--踢掉非活跃连接)
5. [Step8：连接对象 + 分次写（EPOLLOUT）](#五step8连接对象--分次写epollout)
6. [Step9：日志系统](#六step9日志系统)
7. [Step10：MySQL 连接池 + 注册 / 登录](#七step10mysql-连接池--注册--登录)
8. [Step11：压力测试与调参](#八step11压力测试与调参)
9. [Step12（可选）：ET、EPOLLONESHOT、Keep-Alive](#九step12可选etepolloneshotkeep-alive)
10. [与 WebServer（linyacool）的关系](#十与-webserverlinyacool的关系)
11. [全文路线图一表](#十一全文路线图一表)

---

## 一、总览：Step5 之后要走多远

### 1.1 Step5 你已经有什么

| 能力 | 说明 |
|------|------|
| epoll | 主线程 `epoll_wait`，同时监视 listen 与多个 client |
| 非阻塞 listen + `accept` 循环 | client 加入 epoll |
| 线程池 | 主线程 `read` 后 `submit`，工作线程解析 + 读 `www/` + `write` + `close` |
| HTTP | 只解析**请求行**，只支持 **GET**，`Connection: close` |
| 静态站 | `www/`、`pathToFile`、`isPathSafe` |

### 1.2 Step5 还缺什么（相对 TinyWebServer）

TinyWebServer 的核心模块可以概括成：

```text
http（状态机 + GET/POST + 读写缓冲）
  → timer（非活跃连接）
  → log（同步 / 异步）
  → CGImysql（连接池 + 注册登录）
  → test_pressure（Webbench）
  → 进阶：ET、EPOLLONESHOT、Keep-Alive、Reactor/Proactor
```

MyWebServer 在 Step5 之后，建议用 **Step6～Step11** 对齐上述能力；**Step12** 为性能与模式进阶。

### 1.3 推荐顺序（一张图）

```text
Step5  线程池 + epoll + GET 静态文件          ← 你在这里
  │
  ▼
Step6  HTTP 状态机 + Header + POST body
  │
  ▼
Step7  定时器（空闲超时关连接）
  │
  ▼
Step8  HttpConnection + 读/写缓冲 + EPOLLOUT
  │
  ▼
Step9  日志（文件 → 异步）
  │
  ▼
Step10 MySQL + /login /register
  │
  ▼
Step11 Webbench 压测
  │
  ▼
Step12（可选）ET / EPOLLONESHOT / Keep-Alive
```

**为什么不是「先 POST 再定时器」以外的乱序？**

| 顺序 | 原因 |
|------|------|
| Step6 在 Step7 前 | POST 依赖 Header 里的 `Content-Length`；状态机也解决「TCP 分包」 |
| Step7 在 Step8 前 | 定时器要连接在 epoll 上「挂一段时间」；Step5 里 read 完就 `removeFd` + 关连接，不利于练定时器 |
| Step8 在 Step9/10 前 | 大响应、登录页 HTML、错误页都要稳定 `write`；日志和数据库会放大调试难度 |
| Step10 在 Step11 前 | 压测需要功能稳定，否则 QPS 低不知道是性能还是 bug |

---

## 二、开工前：建议先做的整理

在写 Step6 之前，建议先完成下面**整理**（不必叫 Step5.5，但会减少后面返工）：

### 2.1 统一入口

| 现状 | 建议 |
|------|------|
| [main.cpp](../main.cpp) 仍是阻塞 `accept`、路由写在 [Httpparase.cpp](../Httpparase.cpp) | 以 [Step5.cpp](../Step5.cpp) 为基准，合并进 `main.cpp` 或拆成 `server.cpp` + `HttpParse.*` |
| 响应头写 `Http/1.1` | 改为标准 `HTTP/1.1` |

### 2.2 为「连接」留位置

Step6 起建议引入 **`HttpConnection`（或 `HttpConn`）** 类，每个 `client_fd` 一个对象，字段至少包括：

```cpp
int fd_;
string read_buf_;           // 尚未解析完的接收数据
size_t read_idx_;           // 已读入缓冲区的末尾（可选，与 Tiny 一致）
ParseState state_;          // 见 Step6
// 稍后 Step8 再加：write_buf_, write_idx_
```

Step5 里用 lambda + 裸 `client_fd` 可以工作，但状态机、定时器、分次写都依赖「连接级状态」，越早抽类越省事。

### 2.3 对照 TinyWebServer 时怎么读代码

| MyWebServer 概念 | TinyWebServer 文件 |
|------------------|-------------------|
| 解析 HTTP | `http/http_conn.h`、`http/http_conn.cpp` |
| 定时器 | `timer/lst_timer.h`、`timer/lst_timer.cpp`，`webserver.cpp` 里 `timer` / `adjust_timer` / `deal_timer` |
| 日志 | `log/log.h`、`log/log.cpp`、`log/block_queue.h` |
| 数据库 | `CGImysql/sql_connection_pool.h`、`sql_connection_pool.cpp` |
| 主循环 | `webserver.cpp` 的 `eventListen`、`eventLoop` |

---

## 三、Step6：HTTP 状态机（请求行 → 头 → 体）+ POST

> **本篇目标：** 不再只解析第一行；按阶段读完整个 HTTP 请求，并支持 **POST**（先实现 `/echo` 回显 body，不接数据库）。  
> **详细教程（零基础指南）：** [06-HTTP状态机指南](./06-HTTP状态机指南.md)  
> **对照 Tiny：** `CHECK_STATE_*`、`parse_request_line`、`parse_headers`、`parse_content`、`process_read`。

---

### 3.1 先搞懂：什么叫「HTTP 状态机」？

「状态机」不是新协议，而是**解析请求时的步骤流程**：用变量记录「当前解析到哪一层」，每收到一点数据就根据当前层决定下一步。

#### 生活类比：办手续

```text
状态 A：填表（请求行）
   ↓ 表填完
状态 B：交材料（Header 一行行）
   ↓ 材料齐；若是 POST 还要看清单上写要交几页纸
状态 C：交正文（Body，按 Content-Length 收 N 字节）
   ↓
办结：可以生成 HTTP 响应了
```

#### HTTP 请求在字节流里长什么样

```http
POST /login HTTP/1.1                    ← 第 1 段：请求行（Step2/5 只做了这里）
Host: 127.0.0.1:8080                    ← 第 2 段：Header（多行）
Content-Type: application/x-www-form-urlencoded
Content-Length: 27
                                        ← 空行 \r\n\r\n 表示 Header 结束
username=abc&passwd=123456              ← 第 3 段：Body（POST 才有）
```

你 Step5 的 `parseRequestLine` 等价于**只做了状态 A**。  
浏览器发 POST 登录时，用户名密码在 **Body**；Body 多长由 Header 里的 **`Content-Length`** 声明。不解析 Header，就无法正确读 Body。

#### TinyWebServer 的三个主状态

（见 `TinyWebServer/http/http_conn.h`）

| 枚举 | 含义 |
|------|------|
| `CHECK_STATE_REQUESTLINE` | 解析 `METHOD /path HTTP/1.1` |
| `CHECK_STATE_HEADER` | 解析 `Host:`、`Content-Length:`、`Connection:` 等 |
| `CHECK_STATE_CONTENT` | 按 `Content-Length` 收齐 Body |

主循环在 `process_read()` 里 `switch (m_check_state)`，根据状态调用 `parse_request_line` / `parse_headers` / `parse_content`。

#### 主状态机 vs 从状态机（Tiny 文档说法）

| 层次 | 做什么 | Tiny 里大致是 |
|------|--------|----------------|
| **从状态机** | 从字节流里**切出一行**（`\r\n` 是否完整） | `parse_line()` |
| **主状态机** | 这一行属于请求行 / 头 / 体，调用对应 `parse_*` | `process_read()` + `switch` |

学习阶段可以**简化从状态机**：在 `read_buf_` 里用 `find("\r\n")` 取行；行不完整就返回「再等数据」。主状态机三态必须自己实现。

#### 和「一次 read 整包」的区别

Step5：

```text
read 一次 → 整个 buffer 当 string → parseRequestLine → 完事
```

问题：

1. TCP 可能**分多次**才把请求发完（半行、半个 Header 常见）。
2. Body 可能很大，一次 `read` 读不满 `Content-Length`。

状态机做法：

```text
每次 read 追加到 read_buf_
调用 process_read()：
  - 能解析几行就解析几行
  - 数据不够 → 返回 NO_REQUEST，fd 继续留在 epoll 上，下次再读
  - 三段都齐 → 进入业务处理（静态文件 / POST echo）
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

#### POST 不是单独一个 `if`

「支持 POST」= 状态机跑通后的结果，尤其是必须经过 **HEADER → CONTENT**，而不是在 `parseRequestLine` 里加一句 `if (method == "POST")`。

---

### 3.2 Step6 要新增 / 修改什么

#### 3.2.1 数据结构

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
    map<string, string> headers;   // 或只存 Content-Length、Connection
    size_t content_length = 0;
    string body;                   // POST body
};
```

连接类中：

```cpp
ParseState state_ = ParseState::RequestLine;
string read_buf_;
```

#### 3.2.2 建议实现的函数

| 函数 | 作用 |
|------|------|
| `append_read(int fd, HttpConnection& conn)` | `read`/`recv` 追加到 `read_buf_` |
| `get_line(conn, line)` | 从缓冲取一行，不够返回 false |
| `parse_request_line(line, req)` | 成功则 `state_ = Header` |
| `parse_headers(line, req)` | 空行：GET → 完成；POST 有长度 → `state_ = Content` |
| `parse_content(conn, req)` | 判断 `read_buf_` 里 body 是否 ≥ `content_length` |
| `process_read(conn)` | 循环取行 + `switch(state_)`，返回是否「请求完整」 |
| `do_request(req)` | GET 走现有静态逻辑；POST `/echo` 回显 body |

#### 3.2.3 Header 至少要认哪些

| Header | 用途 |
|--------|------|
| `Host` | 规范要求；可先存不用 |
| `Content-Length` | POST body 长度（必须） |
| `Connection: keep-alive` | Step12 再用；Step6 可解析但仍 `close` |

Tiny 在 `parse_headers` 里用 `strncasecmp` 匹配；你可 `find(':')` 再转小写比较 key。

#### 3.2.4 与 Step5 主循环的衔接变化

Step5 在 client 可读时：`read` 一次 → `removeFd` → 扔线程池。

Step6 建议改为：

```text
client 可读
  → append_read
  → process_read
      若未完成：保持 on epoll，继续等读
      若完成：再 submit 到线程池做 do_request + write + close
      （或仍在主线程 write，视你 Step8 设计而定）
```

这样「半包」才不会丢。

---

### 3.3 Step6 验收标准

```bash
# GET 仍正常
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/about.html

# GET /echo → 应返回 echo.html 测试页（不是 404）
curl http://127.0.0.1:8080/echo
# 或浏览器打开 http://127.0.0.1:8080/echo ，提交表单测 POST

# POST echo（curl 快速验收）
curl -X POST http://127.0.0.1:8080/echo -d "hello=world"
# 期望：200，body 里能看到提交的字符串

# 分包：可用小工具或 sleep 发送，确认两次 read 也能拼对（进阶自测）
```

**说明：** 同一路径 `/echo`，**GET** 读 `www/echo.html` 展示表单，**POST** 才回显 body；地址栏只能 GET。详见 [06-HTTP状态机指南](./06-HTTP状态机指南.md) 1.6 节。

---

### 3.4 Step6 易错点

| 错误 | 后果 |
|------|------|
| 只判断 `method == POST"` 不读 body | 登录永远拿不到用户名密码 |
| 用 `strlen` 算 body 长度 | 二进制或带 `\0` 会错；必须用 `Content-Length` |
| Header 没读完就 `do_request` | POST body 粘在 Header 后面被当垃圾 |
| 空行识别错（只认 `\n` 不认 `\r\n`） | 永远进不了 CONTENT 状态 |
| `Content-Length` 很大仍一次性 `string` | 内存爆；大 body 要限长（如 1MB） |

---

### 3.5 Step6 小结

| 项目 | 内容 |
|------|------|
| 核心概念 | HTTP 状态机 = 请求行 → 头 → 体 三阶段解析 |
| 核心代码 | `ParseState` + `process_read` + `parse_*` 三函数 |
| 第一个 POST 业务 | `/echo` 回显 + **`www/echo.html` 表单页** | 不要一上来接 MySQL |
| Tiny 对照 | `http_conn.cpp` 中 `process_read`、`parse_request_line`、`parse_headers`、`parse_content` |

---

## 四、Step7：定时器 —— 踢掉非活跃连接

> **本篇目标：** 长时间不占 CPU 但占着 `fd` 的连接（慢客户端、恶意半连接）能被自动 `close`。  
> **详细教程（零基础指南）：** [07-定时器指南](./07-定时器指南.md)  
> **对照 Tiny：** `timer/lst_timer.*`、`webserver.cpp` 的 `timer`、`adjust_timer`、`deal_timer`、`utils.tick()`。

---

### 4.1 为什么要定时器？

| 场景 | 没有定时器 |
|------|----------|
| 客户端连上后只发一半请求 | `fd` 和 epoll 条目一直占着 |
| 慢速发送 | 阻塞其它连接资源 |
| 攻击 | 容易把 `fd` 耗尽 |

Tiny 在 **accept 成功** 时为该 `connfd` 创建一个定时结点，默认约 `3 * TIMESLOT` 秒后过期；**每次成功读/写** 调用 `adjust_timer` 延后；过期执行回调 **关 socket、从 epoll 删除**。

---

### 4.2 实现思路（由简到难）

#### 方案 A：最小可行（推荐先做）

- 每个连接记录 `last_active = time(nullptr)`。
- 每次 `epoll_wait` 返回后（或循环末尾），扫描所有连接，若 `now - last_active > TIMEOUT_SEC` 则 `close` + `epoll_ctl(DEL)`。
- 每次 `read`/`write` 成功更新 `last_active`。

不必先用 `alarm` + 信号 + 管道（Tiny 的「统一事件源」），逻辑先跑通。

#### 方案 B：对齐 Tiny

- 升序链表 / 最小堆管理 `util_timer`，`expire` 为绝对时间。
- `tick()` 弹出所有过期任务。
- 用 `socketpair` + `SIGALRM` 周期唤醒主循环（见 Tiny `timer/README.md`）。

---

### 4.3 Step7 要改的工程点

1. **连接不要 read 一次就永久摘掉**（Step6 已建议保持 epoll 直到请求完整；Step7 还要求完整前也有超时）。
2. 在 `HttpConnection` 上增加 `time_t last_active_` 或 `util_timer*`。
3. `accept` 时登记定时器；`process_read` / `write` 成功时刷新。
4. 超时回调：`epoll_ctl(DEL, fd)` + `close(fd)` + 从连接表删除。

---

### 4.4 验收标准

```bash
# 用 nc 连上后不发数据，等超过 TIMEOUT（如 15s），连接应被服务器关闭
nc 127.0.0.1 8080
# （等待）→ 连接断开

# 正常 curl 仍应在超时前完成
curl http://127.0.0.1:8080/
```

---

### 4.5 易错点

| 错误 | 后果 |
|------|------|
| 定时器回调里重复 `close` | 崩溃；关之前检查 fd 有效 |
| 已关 fd 仍留在 epoll | `epoll_wait` 报错或忙等 |
| 工作线程 `close` 主线程仍 epoll 等 | 竞态；关 fd 和 epoll 删除要约定在同一线程或加锁 |

---

## 五、Step8：连接对象 + 分次写（EPOLLOUT）

> **本篇目标：** 大文件或慢网络时，`write` 一次写不完也能正确发完；连接读写有固定缓冲区。  
> **详细教程（零基础指南）：** [08-分次写指南](./08-分次写指南.md)  
> **对照 Tiny：** `http_conn::read`、`process_write`、`modfd` 加 `EPOLLOUT`、`m_write_buf`。

---

### 5.1 Step5/6 的写有什么问题？

```cpp
write(client_fd, response.c_str(), response.size());  // 假定一次写完
```

实际上 `write` 可能：

- 返回值 **小于** 要写的长度（内核发送缓冲区满）
- 返回 `-1` 且 `errno == EAGAIN`（非阻塞 fd）

大静态文件（几 MB 图片）时，一次 `string` 全进内存 + 一次 `write` 也不合适。

---

### 5.2 要做什么

| 组件 | 说明 |
|------|------|
| `read_buf_` / `write_buf_` | 读缓冲、待发送响应缓冲 |
| `read_idx_` / `write_idx_` | 已读 / 已写字节数（可选） |
| `process_write()` | 循环 `write`，未写完则 `epoll_ctl(MOD, EPOLLOUT)` |
| `handle_write` 事件 | `EPOLLOUT` 就绪时继续 `process_write` |
| 写完 | 取消 `EPOLLOUT`，`close` 或 Keep-Alive（Step12） |

流程：

```text
do_request 生成响应到 write_buf_
  → 尝试 write
  → 若 EAGAIN 或 未写完：监听 EPOLLOUT
  → 下次 epoll 触发写事件，继续写
  → 写完：关连接或进入下一请求（Keep-Alive）
```

---

### 5.3 与线程池的分工（两种选型）

| 模型 | 说明 |
|------|------|
| **A（贴近你 Step5）** | 主线程 epoll 负责 read/write 事件；线程池只做「解析 + 读盘 + 拼 write_buf_」 |
| **B（贴近 Tiny Proactor）** | 主线程 read；线程池拼响应；主线程 write（Tiny 部分模式） |

选一种后全程统一，避免主线程和工作线程同时 `write` 同一 fd。

---

### 5.4 大文件优化（进阶）

在 Step8 稳定后可选：

- `sendfile(fd_out, file_fd, ...)` 减少用户态拷贝
- 响应头用 `writev` 一次发 header + 文件块

Tiny 曾专门修过「大文件」相关 bug， Worth 对照 `http_conn.cpp` 里写静态文件的逻辑。

---

### 5.5 验收标准

```bash
# 小页面正常
curl http://127.0.0.1:8080/

# 大文件（在 www 放 5MB+ 文件）
curl -O http://127.0.0.1:8080/big.bin
# 大小与磁盘一致

# 可选：非阻塞 + 故意慢读客户端，服务仍不崩
```

---

## 六、Step9：日志系统

> **本篇目标：** 用 `LOG_INFO` 等替代零散 `cout`；高并发时日志不严重拖慢请求线程。  
> **详细教程（零基础指南）：** [09-日志系统指南](./09-日志系统指南.md)  
> **对照 Tiny：** `log/log.*`、`log/block_queue.h`。

---

### 6.1 分两小步

#### Step9a：同步日志

- 单例 `Log::get_instance()`。
- `LOG_INFO(fmt, ...)` 写文件（如 `logs/server.log`）。
- 日志行带时间、级别、内容。
- 命令行或宏控制是否写盘（Tiny 的 `close_log`）。

#### Step9b：异步日志（对齐 Tiny）

- 前端（业务线程）：日志放进**阻塞队列**。
- 后端（单独 log 线程）：批量写文件。
- 双缓冲 / 多缓冲减少锁竞争（Tiny README：4 个缓冲区思想）。

---

### 6.2 为什么异步

写磁盘慢；若在 epoll 线程里同步 `fprintf`，会拖住所有连接。异步 = **业务只投递字符串，慢活交给 log 线程**。

---

### 6.3 验收标准

- 访问几次页面后，日志文件有对应请求记录。
- 压测时开日志 QPS 下降、关日志 QPS 上升（Step11 可量化）。

---

## 七、Step10：MySQL 连接池 + 注册 / 登录

> **本篇目标：** 浏览器表单 POST 到服务器，服务器查库校验用户。  
> **详细教程（零基础指南）：** [10-MySQL登录注册指南](./10-MySQL登录注册指南.md)  
> **对照 Tiny：** `CGImysql/sql_connection_pool.*`、`http_conn::initmysql_result`、`do_request` 里 CGI 分支、`root/register.html`、`log.html`。

---

### 7.1 前置条件

- **Step6** 必须完成：POST body 能完整收到。
- 本机安装 MySQL，建库建表（Tiny README 里的 `yourdb`、`user` 表）。

---

### 7.2 要实现的内容

| 模块 | 说明 |
|------|------|
| 连接池 | 预先创建 N 条 `MYSQL*` 连接，用时取、用完还 |
| RAII | `connectionRAII` 构造取连接、析构还连接（Tiny 做法） |
| 启动加载 | `SELECT username,passwd FROM user` 进 `map<string,string>`（或注册时直接 INSERT） |
| 路由 | `POST /login`、`POST /register` 解析 `username=xx&passwd=yy` |
| 静态页 | `register.html`、`log.html` 表单 action 指向上述接口 |
| 响应 | 成功/失败跳转不同 HTML（Tiny 的 `log.html`、`registerError.html`） |

---

### 7.3 安全提醒（学习项目也要知道）

- 明文密码、拼接 SQL 仅适合本地学习；生产要用哈希密码、预编译语句（防 SQL 注入）。
- 不要把数据库密码提交到公开 Git。

---

### 7.4 验收标准

```bash
# 浏览器打开 http://127.0.0.1:8080/register.html 注册
# 再打开 log.html 登录
# 成功跳转 judge.html 或类似欢迎页（与你实现的页面一致即可）
```

---

## 八、Step11：压力测试与调参

> **本篇目标：** 用数据说明「线程数、日志、定时器」对性能的影响。  
> **详细教程（零基础指南）：** [11-压力测试指南](./11-压力测试指南.md)  
> **对照 Tiny：** `test_pressure/`、`webbench`。

---

### 8.1 工具

- 使用 Tiny 自带的 **webbench** 或自行编译。
- 典型命令形态：`webbench -c 1000 -t 5 http://127.0.0.1:8080/index.html`（以你环境为准）。

---

### 8.2 建议记录的对比实验

| 实验 | 变量 |
|------|------|
| 1 | 线程池 2 / 4 / 8 |
| 2 | 开同步日志 vs 关日志 |
| 3 | 开定时器 vs 关定时器（影响应较小） |
| Step12 后 | LT vs ET、Keep-Alive on/off |

记录：**QPS**、**成功率**、是否出现 `fd` 耗尽或大量超时。

---

### 8.3 验收标准

- 本机能在关日志时达到合理 QPS（不追求复现 Tiny 九万，但要数量级稳定）。
- 压测后无大量 `close` 失败、无内存持续上涨。

---

## 九、Step12（可选）：ET、EPOLLONESHOT、Keep-Alive

> **本篇目标：** 理解 Tiny README 里 Reactor/Proactor、LT/ET 组合；进阶并发。  
> **对照 Tiny：** `config` 里 `trigmode`、`actor_model`；`addfd` / `modfd` 的 `EPOLLET`、`EPOLLONESHOT`。

---

### 9.1 EPOLLET（边沿触发）

- **accept**：必须 `while (accept)` 直到 `EAGAIN`。
- **read**：必须循环 `recv` 直到 `EAGAIN`。
- 否则丢事件，表现为偶发请求无响应。

### 9.2 EPOLLONESHOT

- 同一 fd 上同一时刻只有一个线程处理读或写，避免多线程重复 `read` 同一连接（Tiny 在线程池模型里使用）。

### 9.3 Keep-Alive

- 解析 `Connection: keep-alive` 后，响应头也带 `Keep-Alive`，**同一 fd 处理多个请求**（循环：读下一个 HTTP 请求）。
- 与定时器关系：连接存活更久，`adjust_timer` 更重要。

---

## 十、与 WebServer（linyacool）的关系

完成 Step6～11 后，你已接近 **经典 TinyWebServer 简历项目** 的功能面。

若还要继续架构升级，可参考同仓库 `Webserver/`（非 Tiny）：

| 能力 | WebServer 做法 |
|------|----------------|
| 多 Reactor | Main accept + Sub `EventLoop` 池 |
| 抽象 | `Channel`、`EventLoop`、`Epoll` |
| 跨线程 | `queueInLoop` + `eventfd` |
| 日志 | muduo 式四缓冲异步日志 |

那是 **Step13+** 的另一条路线，不必与 Tiny 路线混在同一步。

---

## 十一、全文路线图一表

| 步骤 | 名称 | 核心产出 | Tiny 主要对照 | 建议验收 |
|------|------|----------|---------------|----------|
| **整理** | 合并 Step5 入口 | `main` 用 epoll+线程池+www | `main.cpp` | 浏览器访问 `www/index.html` |
| **Step6** | HTTP 状态机 + POST | 三态解析、`/echo` | `http_conn.cpp` `process_read` | `curl -X POST /echo -d 'a=b'` |
| **Step7** | 定时器 | 空闲断连 | `lst_timer`、`deal_timer` | `nc` 挂起后自动断开 |
| **Step8** | 分次写 | `write_buf_`、`EPOLLOUT` | `process_write`、`modfd` | 大文件下载完整 |
| **Step9** | 日志 | 文件日志、可选异步 | `log/` | 日志文件有请求记录 |
| **Step10** | MySQL | 注册登录 | `CGImysql/` | 浏览器注册登录成功 |
| **Step11** | 压测 | QPS 数据 | `test_pressure/` | webbench 稳定成功率 |
| **Step12** | 进阶 IO | ET、Keep-Alive 等 | `config` trigmode | 对比实验记录 |

---

## 附录 A：Step6 伪代码骨架（默写参考）

```cpp
enum class ParseState { RequestLine, Header, Content };

bool HttpConnection::process_read() {
    string line;
    while (get_line(read_buf_, line)) {
        switch (state_) {
        case ParseState::RequestLine:
            if (!parse_request_line(line, request_)) return false;
            state_ = ParseState::Header;
            break;
        case ParseState::Header:
            if (line.empty() || line == "\r") { /* 空行 */
                if (request_.method == "POST" && request_.content_length > 0)
                    state_ = ParseState::Content;
                else
                    return true;  // GET 完整
            } else {
                parse_header_line(line, request_);
            }
            break;
        case ParseState::Content:
            if (read_buf_.size() >= header_end + request_.content_length)
                return true;
            return false;  // body 未收齐
        }
    }
    return false;  // 需要继续 read
}
```

（实际要从缓冲区分出「头结束位置」；上面是思路，实现时与 `read_idx_`、`checked_idx` 对齐 Tiny 更稳。）

---

## 附录 B：推荐文件组织

**学习阶段（Step1～StepN，本篇路线 Step7～12 亦然）：**

```text
MyWebServer/                    ← 在此目录编译、./server
├── Step1to6/                   ← 各步单文件 cpp（避免 step 命名搞混）
│   ├── Step1.cpp … Step5.cpp
│   ├── step6.cpp
│   └── step7.cpp …（Step8+ 继续放这里）
├── www/
├── server                      ← g++ -o server 生成在根目录
└── docs/
```

**全部步骤完成后再拆分（可选）：**

```text
MyWebServer/
  src/           ← 多文件源码目录（方法 B），见 [分文件编写指南](./分文件编写指南/00-总览与工程结构.md)
  server         ← 编译产物（与 src/ 目录分离，避免同名冲突）
  Makefile
  src/HttpConnection.h/cpp  （等模块均在 src/ 下）
  src/ThreadPool.h/cpp
  src/Log.h/cpp
  …
```

**编译约定：** 在**项目根目录**执行，例如：

```bash
g++ -std=c++17 -Wall -pthread -o server Step1to6/step7.cpp
./server
```

（需根目录有 `www/`。）

---

## 附录 C：常见问题

**Q：能否跳过 Step7 直接做 MySQL？**  
能跑通但不稳；慢连接会占满 fd，排错也难。

**Q：状态机和「解析 POST」是一回事吗？**  
POST 是状态机的一种用法；必须先有 Header 态才能正确读 body。

**Q：要和 Tiny 一模一样吗？**  
不必。顺序和能力对齐即可；实现可更简单（如定时器用扫描代替信号）。

**Q：Windows 能练吗？**  
网络部分建议 **WSL2 / Linux**；epoll、webbench、MySQL 在 Windows 原生上不便。

---

> **下一步建议：** 按 **Step11** 在 `Step1to6/step11.cpp`（step10 + 命令行调参）做 Webbench 压测，记录 QPS。详见 [11-压力测试指南](./11-压力测试指南.md)。主路线 Step6～11 完成后，可选 [Step12](#九step12可选etepolloneshotkeep-alive) 进阶 IO。
