# MySQL 连接池 + 注册 / 登录 —— 零基础学习指南（第十篇）

> **写给谁看：** 已完成 [第九篇](./09-日志系统指南.md)（`step9.cpp` / `Step1to6/step9.cpp`）的学习者——会用状态机收 POST body、异步日志、eventfd 唤醒。  
> **本篇目标：** 浏览器表单 **POST** 到 `/register`、`/login`，服务器用 **MySQL 连接池** 写库 / 查库，成功则 **302 跳转** 欢迎页。  
> **不做的事：** 不做密码哈希、不做预编译语句（生产必须做）—— 本篇是**本地学习版**；不要把数据库密码提交到 Git。  
> **代码形式：** 一个 cpp 写到底，保存为 **`Step1to6/step10.cpp`**；编译在**项目根目录**，需链接 **`-lmysqlclient`**。  
> **前置文档：** [06-HTTP状态机指南](./06-HTTP状态机指南.md) · [09-日志系统指南](./09-日志系统指南.md) · [Step5 之后路线图](./Step5之后完善路线图.md)  
> **代码疑问：** [10-Step10代码深度答疑](./10-Step10代码深度答疑.md)（`SqlPool`、`url_decode`、`handle_login` 等逐行讲解）

---

## 目录

1. [第一部分：先搞懂「我们在升级什么」](#第一部分先搞懂我们在升级什么)
2. [第二部分：MySQL 环境与建表](#第二部分mysql-环境与建表)
3. [第三部分：www 表单页面](#第三部分www-表单页面)
4. [第四部分：需要哪些新「工具」](#第四部分需要哪些新工具)
5. [第五部分：完整代码 + 逐块讲解](#第五部分完整代码--逐块讲解)
6. [编译、运行与测试](#编译运行与测试)
7. [常见问题排查](#常见问题排查)
8. [本篇小结与下一篇预告](#本篇小结与下一篇预告)

---

## 第一部分：先搞懂「我们在升级什么」

### 1.1 Step9 能做什么、不能做什么

| 已有 | 还没有 |
|------|--------|
| POST `/echo` 回显 body | **持久化用户账号** |
| 静态页 `www/` | **注册 / 登录业务** |
| 线程池里 `do_request` | **访问 MySQL** |

真实网站：用户填表单 → 服务器收 POST body → **查数据库** → 返回成功或失败页。

Step6 状态机保证 body 完整；Step10 在 `do_request` 里增加 **`/register`、`/login`** 分支。

---

### 1.2 生活类比：会员登记

```text
register.html  = 填「用户名 + 密码」登记表
POST /register = 服务员把表交给后台入库（INSERT）

log.html       = 登录窗口
POST /login    = 查账本有没有这个人、密码对不对（SELECT）

连接池         = 前台固定准备 8 本电话线连数据库
                 用完还回去，不用每次重新拨号（mysql_real_connect 很贵）
```

---

### 1.3 一次注册 / 登录的完整路径

```text
浏览器 GET /register.html     → 静态 HTML 表单
浏览器 POST /register           → body: username=abc&passwd=123456
  → 状态机收齐 body
  → 工作线程 do_request
  → SqlPool 取连接 → INSERT
  → 302 Location: /welcome.html

浏览器 GET /log.html            → 登录表单
浏览器 POST /login              → SELECT 比对 password 列
  → 成功 302 /welcome.html
  → 失败 302 /login_error.html
```

---

### 1.4 本篇架构

```text
┌─────────────────────────────────────────────────────────────┐
│  epoll 主线程 + eventfd + 分次写（同 Step9）                   │
└───────────────────────────┬─────────────────────────────────┘
                            │ 线程池
                            ▼
                   do_request(req)
                     ├─ GET  → www 静态文件
                     ├─ POST /echo
                     ├─ POST /register  → SqlPool → INSERT
                     └─ POST /login     → SqlPool → SELECT
                            │
                            ▼
                      MySQL Server
                      库 webserver 表 user
```

---

### 1.5 与 Step9 的区别

| 项目 | Step9 | Step10 |
|------|-------|--------|
| POST 业务 | `/echo` | + **`/register` `/login`** |
| 存储 | 无 | **MySQL** |
| 连接池 | 无 | **`SqlPool` + RAII** |
| www 页面 | index / echo | + **register / log / welcome** |
| 编译 | `-pthread` | + **`-lmysqlclient`** |

---

### 1.6 对照 TinyWebServer

| 本篇 | Tiny |
|------|------|
| `SqlPool` | `sql_connection_pool` |
| `ConnectionRAII` | `connectionRAII` |
| `POST /login` | `http_conn` CGI 分支 |
| `register.html` | `root/register.html` |

---

### 1.7 安全提醒（必读）

| 学习项目做法 | 生产环境必须 |
|-------------|-------------|
| 密码明文存库 | bcrypt / Argon2 哈希 |
| 字符串拼接 SQL | **预编译语句**（防 SQL 注入） |
| 密码写在 cpp 常量 | 环境变量 / 配置文件，**不进 Git** |

本篇对用户名做简单 `'` 转义，**仅用于本机练习**。

---

## 第二部分：MySQL 环境与建表

### 2.1 安装（WSL / Ubuntu）

```bash
sudo apt update
sudo apt install mysql-server libmysqlclient-dev
sudo service mysql start
```

### 2.2 建库建表

登录 MySQL：

```bash
sudo mysql -u root -p
```

执行：

```sql
CREATE DATABASE IF NOT EXISTS webserver DEFAULT CHARSET utf8mb4;
USE webserver;

CREATE TABLE IF NOT EXISTS user (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(50) NOT NULL
);
```

> **表单字段 vs 数据库列名（别混）：**  
> - HTML 表单、POST body 里密码字段名是 **`passwd`**（`name="passwd"` → `username=abc&passwd=123456`）  
> - MySQL 表里密码列名是 **`password`**  
> - 代码里：`form["passwd"]` 读表单，`INSERT ... password` / `SELECT password` 写库查库  

验证：

```sql
SELECT * FROM user;
```

### 2.3 修改 step10.cpp 里的数据库配置

在源码顶部（**不要提交真实密码到公开仓库**）：

```cpp
// 注意：#include <mysql/mysql.h> 之后，MYSQL_PORT 等名字是头文件里的宏，
// 不能再用 const int MYSQL_PORT = 3306; 否则会编译报错。
const char* DB_HOST = "127.0.0.1";
const int   DB_PORT = 3306;
const char* DB_USER = "root";
const char* DB_PASS = "你的密码";
const char* DB_NAME = "webserver";
const size_t DB_POOL_SIZE = 8;
```

---

## 第三部分：www 表单页面

在 `www/` 下新建以下文件（也可从本篇复制）。

### 3.1 `www/register.html`

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>用户注册</title>
  <link rel="stylesheet" href="/css/style.css">
</head>
<body>
  <h1>用户注册</h1>
  <form action="/register" method="post">
    <p>用户名：<input name="username" required maxlength="50"></p>
    <p>密码：<input name="passwd" type="password" required maxlength="50"></p>
    <button type="submit">注册</button>
  </form>
  <p><a href="/log.html">已有账号？去登录</a> · <a href="/">首页</a></p>
</body>
</html>
```

### 3.2 `www/log.html`

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>用户登录</title>
  <link rel="stylesheet" href="/css/style.css">
</head>
<body>
  <h1>用户登录</h1>
  <form action="/login" method="post">
    <p>用户名：<input name="username" required></p>
    <p>密码：<input name="passwd" type="password" required></p>
    <button type="submit">登录</button>
  </form>
  <p><a href="/register.html">没有账号？去注册</a> · <a href="/">首页</a></p>
</body>
</html>
```

### 3.3 `www/welcome.html`

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>登录成功</title>
  <link rel="stylesheet" href="/css/style.css">
</head>
<body>
  <h1>欢迎！</h1>
  <p>你已成功登录或注册。</p>
  <p><a href="/">返回首页</a></p>
</body>
</html>
```

### 3.4 `www/login_error.html` / `www/register_error.html`

```html
<!-- login_error.html -->
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="utf-8"><title>登录失败</title>
<link rel="stylesheet" href="/css/style.css"></head>
<body>
  <h1>登录失败</h1>
  <p>用户名或密码错误。</p>
  <p><a href="/log.html">重试</a></p>
</body>
</html>
```

```html
<!-- register_error.html -->
<!DOCTYPE html>
<html lang="zh-CN">
<head><meta charset="utf-8"><title>注册失败</title>
<link rel="stylesheet" href="/css/style.css"></head>
<body>
  <h1>注册失败</h1>
  <p>用户名可能已存在，或输入不合法。</p>
  <p><a href="/register.html">重试</a></p>
</body>
</html>
```

### 3.5 更新 `www/index.html` 链接

在首页 `<ul>` 里增加：

```html
<li><a href="/register.html">注册</a></li>
<li><a href="/log.html">登录</a></li>
```

---

## 第四部分：需要哪些新「工具」

### 4.1 新增头文件

```cpp
#include <mysql/mysql.h>
```

链接：`-lmysqlclient`

### 4.2 `SqlPool` 连接池

```cpp
class SqlPool {
    queue<MYSQL*> pool_;
    mutex mtx_;
    condition_variable cv_;
public:
    static SqlPool& instance();
    void init(host, port, user, pass, db, poolSize);
    MYSQL* get();      // 取连接（池空则等待）
    void release(MYSQL*);  // 还连接
};
```

| 方法 | 作用 |
|------|------|
| `init` | 预先创建 N 条 `mysql_real_connect` |
| `get` | 工作线程取一条连接 |
| `release` | 用完放回队列 |

### 4.3 `ConnectionRAII`（Tiny 同款思路）

```cpp
struct ConnectionRAII {
    MYSQL* conn;
    ConnectionRAII() : conn(SqlPool::instance().get()) {}
    ~ConnectionRAII() { SqlPool::instance().release(conn); }
};
```

构造时 `get`，析构时 `release`，**防止忘记还连接**。

### 4.4 表单解析

POST body 格式：`username=abc&passwd=123456`（`passwd` 是**表单字段名**，不是数据库列名）

```cpp
map<string, string> parse_form_urlencoded(const string& body);
string url_decode(const string& s);   // + → 空格，简单 %XX
string escape_sql(const string& s); // 仅学习：把 ' 变成 ''
```

注册 / 登录 SQL 使用表列名 **`password`**：

```cpp
string pass = form["passwd"];  // 从 POST body 读密码
// INSERT INTO user(username, password) VALUES(...)
// SELECT password FROM user WHERE username='...'
```

### 4.5 HTTP 302 跳转

```cpp
string buildRedirectResponse(const string& location) {
    return "HTTP/1.1 302 Found\r\n"
           "Location: " + location + "\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n";
}
```

浏览器收到 302 会自动 GET `Location` 里的页面。

### 4.6 相对 Step9 改动清单

| 序号 | 改动 |
|------|------|
| 1 | `#include <mysql/mysql.h>` + `SqlPool` + `ConnectionRAII` |
| 2 | `parse_form_urlencoded` / `escape_sql` |
| 3 | `handle_register` / `handle_login` |
| 4 | `buildRedirectResponse` |
| 5 | `do_request` 增加 POST 分支 |
| 6 | `pathToFile` 映射新 HTML |
| 7 | `main` 里 `SqlPool::instance().init(...)` |
| 8 | Step9 的日志 / eventfd / 分次写 **全部保留** |

---

## 第五部分：完整代码 + 逐块讲解

### 5.1 完整代码

保存为 **`Step1to6/step10.cpp`**（在 Step9 基础上改）。

> 代码较长：Step9 主体 + Step10 MySQL 模块。默写时在 step9 上按 §4.6 打补丁即可。

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
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mysql/mysql.h>

using namespace std;

const string WEB_ROOT = "www";
const string LOG_PATH = "logs/server.log";
const bool LOG_ASYNC = true;
const int MAX_EVENTS = 64;
const int PORT = 8080;
const size_t THREAD_POOL_SIZE = 4;
const size_t MAX_BODY_SIZE = 1024 * 1024;
const int CONN_TIMEOUT_SEC = 15;

// ========== 数据库配置（本机修改，勿提交 Git）==========
// mysql.h 里已有 MYSQL_PORT 等宏，变量名用 DB_* 避免冲突
const char* DB_HOST = "127.0.0.1";
const int   DB_PORT = 3306;
const char* DB_USER = "root";
const char* DB_PASS = "your_password";
const char* DB_NAME = "webserver";
const size_t DB_POOL_SIZE = 8;

enum class LogLevel { Debug, Info, Warn, Error };
enum class ParseState { RequestLine, Header, Content };
enum class ReadResult { Incomplete, Complete, Error };
enum class WriteResult { Incomplete, Complete, Error };
enum class ConnPhase { Reading, WaitingResponse, Writing };

// ========== Step9：Log（与 step9 相同，略）==========
class Log {
public:
    static Log& instance() { static Log inst; return inst; }
    void init(const string& path, bool async) {
        async_ = async;
        file_.open(path, ios::app);
        if (!file_) { cerr << "无法打开日志: " << path << "\n"; return; }
        if (async_) { stop_ = false; worker_ = thread([this]() { log_thread_loop(); }); }
    }
    void write(LogLevel level, const string& msg) {
        string line = format_line(level, msg);
        if (async_) write_async(line); else write_sync(line);
    }
    ~Log() {
        if (async_ && worker_.joinable()) {
            { lock_guard<mutex> lock(mtx_); stop_ = true; }
            cv_.notify_all(); worker_.join();
        }
        if (file_.is_open()) file_.close();
    }
private:
    Log() = default;
    string format_line(LogLevel level, const string& msg) {
        time_t now = time(nullptr);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        const char* tag = "INFO";
        switch (level) {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info:  tag = "INFO";  break;
        case LogLevel::Warn:  tag = "WARN";  break;
        case LogLevel::Error: tag = "ERROR"; break;
        }
        return string(tbuf) + " [" + tag + "] " + msg;
    }
    void write_sync(const string& line) {
        lock_guard<mutex> lock(mtx_);
        if (file_.is_open()) { file_ << line << '\n'; file_.flush(); }
    }
    void write_async(const string& line) {
        { lock_guard<mutex> lock(mtx_); queue_.push(line); }
        cv_.notify_one();
    }
    void log_thread_loop() {
        while (true) {
            string line;
            { unique_lock<mutex> lock(mtx_);
              cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
              if (stop_ && queue_.empty()) return;
              line = move(queue_.front()); queue_.pop(); }
            if (file_.is_open()) { file_ << line << '\n'; file_.flush(); }
        }
    }
    ofstream file_;
    bool async_ = true, stop_ = false;
    queue<string> queue_;
    mutex mtx_;
    condition_variable cv_;
    thread worker_;
};
#define LOG_INFO(msg)  Log::instance().write(LogLevel::Info,  msg)
#define LOG_WARN(msg)  Log::instance().write(LogLevel::Warn,  msg)
#define LOG_ERROR(msg) Log::instance().write(LogLevel::Error, msg)

// ========== Step10：MySQL 连接池 ==========
class SqlPool {
public:
    static SqlPool& instance() { static SqlPool p; return p; }

    void init(const char* host, int port, const char* user,
              const char* pass, const char* db, size_t poolSize) {
        for (size_t i = 0; i < poolSize; ++i) {
            MYSQL* conn = create_connection(host, port, user, pass, db);
            if (!conn) throw runtime_error("MySQL 连接池初始化失败");
            pool_.push(conn);
        }
        LOG_INFO("MySQL 连接池就绪 size=" + to_string(poolSize));
    }

    MYSQL* get() {
        unique_lock<mutex> lock(mtx_);
        cv_.wait(lock, [this]() { return !pool_.empty(); });
        MYSQL* conn = pool_.front();
        pool_.pop();
        return conn;
    }

    void release(MYSQL* conn) {
        if (!conn) return;
        lock_guard<mutex> lock(mtx_);
        pool_.push(conn);
        cv_.notify_one();
    }

private:
    SqlPool() = default;

    MYSQL* create_connection(const char* host, int port,
                             const char* user, const char* pass, const char* db) {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) return nullptr;
        if (!mysql_real_connect(conn, host, user, pass, db, port, nullptr, 0)) {
            LOG_ERROR(string("mysql_real_connect: ") + mysql_error(conn));
            mysql_close(conn);
            return nullptr;
        }
        mysql_set_character_set(conn, "utf8mb4");
        return conn;
    }

    queue<MYSQL*> pool_;
    mutex mtx_;
    condition_variable cv_;
};

struct ConnectionRAII {
    MYSQL* conn;
    ConnectionRAII() : conn(SqlPool::instance().get()) {}
    ~ConnectionRAII() { SqlPool::instance().release(conn); }
};

// ========== Step6～9：HTTP + 连接（结构同 step9）==========
struct HttpRequest {
    string method, path, version, body;
    map<string, string> headers;
    size_t content_length = 0;
};

class HttpConnection {
public:
    explicit HttpConnection(int fd) : fd_(fd), last_active_(time(nullptr)) {}
    int fd_;
    ParseState state_ = ParseState::RequestLine;
    string read_buf_;
    HttpRequest request_;
    time_t last_active_;
    ConnPhase phase_ = ConnPhase::Reading;
    string write_buf_;
    size_t write_idx_ = 0;
    void refresh_active() { last_active_ = time(nullptr); }
    bool is_expired(int t) const { return difftime(time(nullptr), last_active_) >= t; }
};

struct ReadyResponse { int fd; string data; };
queue<ReadyResponse> g_ready_queue;
mutex g_ready_mtx;
int g_notify_fd = -1;

void wake_main_thread() {
    if (g_notify_fd < 0) return;
    uint64_t one = 1;
    write(g_notify_fd, &one, sizeof(one));
}

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}
static string toLower(string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool get_line(string& buf, string& line) {
    size_t p = buf.find("\r\n");
    if (p == string::npos) return false;
    line = buf.substr(0, p); buf.erase(0, p + 2);
    return true;
}
bool parse_request_line(const string& line, HttpRequest& req) {
    istringstream iss(line);
    return static_cast<bool>(iss >> req.method >> req.path >> req.version);
}
void parse_header_line(const string& line, HttpRequest& req) {
    size_t c = line.find(':');
    if (c == string::npos) return;
    string k = toLower(trim(line.substr(0, c)));
    string v = trim(line.substr(c + 1));
    req.headers[k] = v;
    if (k == "content-length") req.content_length = static_cast<size_t>(stoul(v));
}

ReadResult process_read(HttpConnection& conn) {
    string line;
    while (true) {
        if (conn.state_ == ParseState::Content) {
            if (conn.request_.content_length > MAX_BODY_SIZE) return ReadResult::Error;
            if (conn.read_buf_.size() >= conn.request_.content_length) {
                conn.request_.body = conn.read_buf_.substr(0, conn.request_.content_length);
                conn.read_buf_.erase(0, conn.request_.content_length);
                return ReadResult::Complete;
            }
            return ReadResult::Incomplete;
        }
        if (!get_line(conn.read_buf_, line)) return ReadResult::Incomplete;
        switch (conn.state_) {
        case ParseState::RequestLine:
            if (!parse_request_line(line, conn.request_)) return ReadResult::Error;
            conn.state_ = ParseState::Header; break;
        case ParseState::Header:
            if (line.empty()) {
                if (conn.request_.method == "POST" && conn.request_.content_length > 0)
                    conn.state_ = ParseState::Content;
                else return ReadResult::Complete;
            } else parse_header_line(line, conn.request_);
            break;
        case ParseState::Content: break;
        }
    }
}

bool append_read(HttpConnection& conn) {
    char buf[8192];
    ssize_t n = read(conn.fd_, buf, sizeof(buf));
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK);
    if (n == 0) return false;
    conn.read_buf_.append(buf, static_cast<size_t>(n));
    return true;
}

string buildHttpResponse(int code, const string& text, const string& type, const string& body) {
    return "HTTP/1.1 " + to_string(code) + " " + text + "\r\n"
           "Content-Type: " + type + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

string buildRedirectResponse(const string& location) {
    return "HTTP/1.1 302 Found\r\nLocation: " + location + "\r\n"
           "Content-Length: 0\r\nConnection: close\r\n\r\n";
}

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html") return WEB_ROOT + "/index.html";
    if (urlPath == "/echo" || urlPath == "/echo.html") return WEB_ROOT + "/echo.html";
    if (urlPath == "/log" || urlPath == "/log.html") return WEB_ROOT + "/log.html";
    if (urlPath == "/register" || urlPath == "/register.html") return WEB_ROOT + "/register.html";
    if (urlPath == "/welcome" || urlPath == "/welcome.html") return WEB_ROOT + "/welcome.html";
    if (urlPath == "/login_error.html") return WEB_ROOT + "/login_error.html";
    if (urlPath == "/register_error.html") return WEB_ROOT + "/register_error.html";
    return WEB_ROOT + urlPath;
}

bool isPathSafe(const string& p) { return p.find("..") == string::npos; }

string getContentType(const string& fp) {
    if (fp.size() >= 5 && fp.substr(fp.size()-5)==".html") return "text/html; charset=utf-8";
    if (fp.size() >= 4 && fp.substr(fp.size()-4)==".css") return "text/css; charset=utf-8";
    return "application/octet-stream";
}

bool readFile(const string& fp, string& out) {
    ifstream ifs(fp, ios::binary);
    if (!ifs) return false;
    out.assign((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());
    return true;
}

string url_decode(const string& s) {
    string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = s[i+1], lo = s[i+2];
            auto hex = [](int c) { return (c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:-1; };
            int h = hex(hi), l = hex(lo);
            if (h >= 0 && l >= 0) { out += static_cast<char>((h<<4)|l); i += 2; }
            else out += s[i];
        } else out += s[i];
    }
    return out;
}

map<string, string> parse_form_urlencoded(const string& body) {
    map<string, string> m;
    size_t start = 0;
    while (start < body.size()) {
        size_t amp = body.find('&', start);
        string pair = body.substr(start, amp == string::npos ? string::npos : amp - start);
        size_t eq = pair.find('=');
        if (eq != string::npos)
            m[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        if (amp == string::npos) break;
        start = amp + 1;
    }
    return m;
}

string escape_sql(const string& s) {
    string out;
    for (char c : s) {
        if (c == '\'') out += "''";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

string buildGetResponse(const HttpRequest& req) {
    if (!isPathSafe(req.path))
        return buildHttpResponse(403, "Forbidden", "text/plain; charset=utf-8", "403");
    string fp = pathToFile(req.path), body;
    if (!readFile(fp, body))
        return buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8", "404: " + req.path);
    return buildHttpResponse(200, "OK", getContentType(fp), body);
}

string handle_register(const HttpRequest& req) {
    auto form = parse_form_urlencoded(req.body);
    string user = form["username"];
    string pass = form["passwd"];
    if (user.empty() || pass.empty())
        return buildRedirectResponse("/register_error.html");

    ConnectionRAII raii;
    MYSQL* conn = raii.conn;
    string sql = "INSERT INTO user(username, password) VALUES('"
               + escape_sql(user) + "','" + escape_sql(pass) + "')";
    if (mysql_query(conn, sql.c_str()) != 0) {
        LOG_WARN(string("注册失败: ") + mysql_error(conn));
        return buildRedirectResponse("/register_error.html");
    }
    LOG_INFO("注册成功 user=" + user);
    return buildRedirectResponse("/welcome.html");
}

string handle_login(const HttpRequest& req) {
    auto form = parse_form_urlencoded(req.body);
    string user = form["username"];
    string pass = form["passwd"];
    if (user.empty() || pass.empty())
        return buildRedirectResponse("/login_error.html");

    ConnectionRAII raii;
    MYSQL* conn = raii.conn;
    string sql = "SELECT password FROM user WHERE username='"
               + escape_sql(user) + "' LIMIT 1";
    if (mysql_query(conn, sql.c_str()) != 0) {
        LOG_ERROR(string("登录查询失败: ") + mysql_error(conn));
        return buildRedirectResponse("/login_error.html");
    }
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) return buildRedirectResponse("/login_error.html");

    MYSQL_ROW row = mysql_fetch_row(res);
    bool ok = row && row[0] && string(row[0]) == pass;
    mysql_free_result(res);

    if (ok) {
        LOG_INFO("登录成功 user=" + user);
        return buildRedirectResponse("/welcome.html");
    }
    LOG_WARN("登录失败 user=" + user);
    return buildRedirectResponse("/login_error.html");
}

string buildPostEchoResponse(const HttpRequest& req) {
    return buildHttpResponse(200, "OK", "text/plain; charset=utf-8",
        "收到 POST body:\n" + req.body);
}

string do_request(const HttpRequest& req) {
    if (req.method == "GET") return buildGetResponse(req);
    if (req.method == "POST") {
        if (req.path == "/echo") return buildPostEchoResponse(req);
        if (req.path == "/register") return handle_register(req);
        if (req.path == "/login") return handle_login(req);
        return buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8",
            "未知 POST: " + req.path);
    }
    return buildHttpResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "405");
}

// ThreadPool / epoll / handle_read / handle_write / drain / tick / main
// ===== 与 step9 完全相同，仅 main 开头增加 SqlPool::init =====

class ThreadPool {
public:
    explicit ThreadPool(size_t n) : stop_(false) {
        for (size_t i = 0; i < n; ++i) workers_.emplace_back([this]() { workerLoop(); });
    }
    ~ThreadPool() {
        { lock_guard<mutex> lk(mtx_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }
    void submit(function<void()> task) {
        { lock_guard<mutex> lk(mtx_); tasks_.push(move(task)); }
        cv_.notify_one();
    }
private:
    void workerLoop() {
        while (true) {
            function<void()> task;
            { unique_lock<mutex> lk(mtx_);
              cv_.wait(lk, [this]() { return stop_ || !tasks_.empty(); });
              if (stop_ && tasks_.empty()) return;
              task = move(tasks_.front()); tasks_.pop(); }
            task();
        }
    }
    queue<function<void()>> tasks_;
    mutex mtx_; condition_variable cv_;
    vector<thread> workers_; bool stop_;
};

void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
void addFd(int epfd, int fd, uint32_t ev = EPOLLIN) {
    epoll_event e{}; e.events = ev; e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e);
}
void modFd(int epfd, int fd, uint32_t ev) {
    epoll_event e{}; e.events = ev; e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
}
void removeFd(int epfd, int fd) { epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr); }

void closeConnection(int epfd, int fd, unordered_map<int, HttpConnection>& conns) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    removeFd(epfd, fd); close(fd); conns.erase(it);
}

void tick_expired_connections(int epfd, unordered_map<int, HttpConnection>& conns) {
    vector<int> v;
    for (auto& [fd, c] : conns)
        if (c.phase_ == ConnPhase::Reading && c.is_expired(CONN_TIMEOUT_SEC)) v.push_back(fd);
    for (int fd : v) { LOG_WARN("超时 fd=" + to_string(fd)); closeConnection(epfd, fd, conns); }
}

WriteResult process_write(int epfd, HttpConnection& c) {
    c.refresh_active();
    while (c.write_idx_ < c.write_buf_.size()) {
        ssize_t n = write(c.fd_, c.write_buf_.data() + c.write_idx_,
                          c.write_buf_.size() - c.write_idx_);
        if (n > 0) { c.write_idx_ += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            modFd(epfd, c.fd_, EPOLLOUT); return WriteResult::Incomplete;
        }
        return WriteResult::Error;
    }
    return WriteResult::Complete;
}

void handle_write(int epfd, HttpConnection& c, unordered_map<int, HttpConnection>& conns) {
    if (c.phase_ != ConnPhase::Writing) return;
    WriteResult w = process_write(epfd, c);
    if (w == WriteResult::Complete || w == WriteResult::Error)
        closeConnection(epfd, c.fd_, conns);
}

void drain_ready_responses(int epfd, unordered_map<int, HttpConnection>& conns) {
    queue<ReadyResponse> local;
    { lock_guard<mutex> lk(g_ready_mtx); swap(local, g_ready_queue); }
    while (!local.empty()) {
        auto rr = move(local.front()); local.pop();
        auto it = conns.find(rr.fd);
        if (it == conns.end()) continue;
        HttpConnection& c = it->second;
        if (c.phase_ != ConnPhase::WaitingResponse) continue;
        c.write_buf_ = move(rr.data); c.write_idx_ = 0; c.phase_ = ConnPhase::Writing;
        modFd(epfd, c.fd_, EPOLLOUT);
        WriteResult w = process_write(epfd, c);
        if (w == WriteResult::Complete || w == WriteResult::Error)
            closeConnection(epfd, c.fd_, conns);
    }
}

void handle_read(int epfd, HttpConnection& c, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& conns) {
    if (c.phase_ != ConnPhase::Reading) return;
    if (!append_read(c)) { closeConnection(epfd, c.fd_, conns); return; }
    c.refresh_active();
    ReadResult r = process_read(c);
    if (r == ReadResult::Incomplete) return;
    if (r == ReadResult::Error) { closeConnection(epfd, c.fd_, conns); return; }
    HttpRequest req = move(c.request_);
    int fd = c.fd_;
    c.phase_ = ConnPhase::WaitingResponse;
    modFd(epfd, fd, 0);
    LOG_INFO("请求完整 " + req.method + " " + req.path);
    pool.submit([fd, req]() {
        string resp = do_request(req);
        { lock_guard<mutex> lk(g_ready_mtx); g_ready_queue.push({fd, move(resp)}); }
        wake_main_thread();
    });
}

int main() {
    Log::instance().init(LOG_PATH, LOG_ASYNC);
    try {
        SqlPool::instance().init(DB_HOST, DB_PORT, DB_USER,
                                 DB_PASS, DB_NAME, DB_POOL_SIZE);
    } catch (const exception& e) {
        cerr << "MySQL 初始化失败: " << e.what() << "\n";
        return 1;
    }

    ThreadPool pool(THREAD_POOL_SIZE);
    unordered_map<int, HttpConnection> connections;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(PORT);
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);
    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);
    addFd(epfd, listen_fd);
    g_notify_fd = eventfd(0, EFD_NONBLOCK);
    addFd(epfd, g_notify_fd);

    cout << "Step10 已启动 http://127.0.0.1:" << PORT << "\n";
    cout << "注册: /register.html  登录: /log.html\n";
    LOG_INFO("Step10 MySQL 服务器启动");

    vector<epoll_event> events(MAX_EVENTS);
    while (true) {
        int n = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        if (n < 0) continue;
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (fd == g_notify_fd) {
                uint64_t u; while (read(g_notify_fd, &u, sizeof(u)) > 0) {}
                drain_ready_responses(epfd, connections); continue;
            }
            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in ca{}; socklen_t len = sizeof(ca);
                    int cfd = accept(listen_fd, (sockaddr*)&ca, &len);
                    if (cfd < 0) { if (errno==EAGAIN||errno==EWOULDBLOCK) break; break; }
                    setNonBlocking(cfd);
                    addFd(epfd, cfd);
                    connections.emplace(cfd, HttpConnection(cfd));
                }
            } else {
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                if (ev & EPOLLIN) handle_read(epfd, it->second, pool, connections);
                if (ev & EPOLLOUT) {
                    it = connections.find(fd);
                    if (it != connections.end()) handle_write(epfd, it->second, connections);
                }
            }
        }
        drain_ready_responses(epfd, connections);
        tick_expired_connections(epfd, connections);
    }
    return 0;
}
```

> **说明：** 上文 `Log` 与 step9 相同；`ThreadPool` / epoll 主循环与 step9 相同。若你已有完整 step9，**只需合并 Step10 段落**（SqlPool、表单解析、handle_register/login、do_request 分支、main 里 init）。

---

### 5.2 逐块讲解：`SqlPool`

```text
init：循环 DB_POOL_SIZE 次 mysql_real_connect → push 进 pool_
get：池空则 wait → pop 一条给业务
release：push 回去 → notify
```

为什么需要池？每次请求都 `mysql_real_connect` 很慢；连接池 **复用** 长连接。

---

### 5.3 逐块讲解：`ConnectionRAII`

```cpp
{
    ConnectionRAII raii;      // 构造：get()
    MYSQL* conn = raii.conn;
    mysql_query(conn, sql);
}                             // 析构：release()，即使中间 return 也会还连接
```

---

### 5.4 逐块讲解：注册 / 登录

**注册：**

```sql
INSERT INTO user(username, password) VALUES('...', '...')
```

用户名重复 → MySQL 报 UNIQUE 错 → 跳转 `/register_error.html`。

**登录：**

```sql
SELECT password FROM user WHERE username='...' LIMIT 1
```

比对 `row[0]` 与表单密码 → 成功 `/welcome.html`，失败 `/login_error.html`。

---

### 5.5 逐块讲解：302 跳转

```http
HTTP/1.1 302 Found
Location: /welcome.html
Content-Length: 0
Connection: close
```

浏览器自动 GET `/welcome.html`，用户看到欢迎页。

---

### 5.6 相对 Step9 对照表

| 项目 | Step9 | Step10 |
|------|-------|--------|
| POST | `/echo` | + `/register` `/login` |
| 持久化 | 无 | MySQL `user` 表 |
| 连接池 | 无 | `SqlPool` |
| 编译 | `-pthread` | + `-lmysqlclient` |

---

## 编译、运行与测试

### 6.1 准备

```bash
mkdir -p logs www
# 确保 MySQL 已建库建表（第二节）
# 创建第三节 HTML 文件
# 修改 step10.cpp 里 DB_PASS
```

### 6.2 编译

在**项目根目录**：

```bash
g++ -std=c++17 -Wall -pthread -o server Step1to6/step10.cpp -lmysqlclient
```

### 6.3 运行

```bash
./server
```

应看到 `Step10 已启动` 和日志里 `MySQL 连接池就绪`。

### 6.4 浏览器验收（核心）

1. 打开 `http://127.0.0.1:8080/register.html`  
2. 注册 `username=test` / `passwd=123456`  
3. 应跳转到 **welcome.html**  
4. 打开 `http://127.0.0.1:8080/log.html`  
5. 用同一账号登录 → 再次进入 welcome  
6. 错误密码 → **login_error.html**

### 6.5 数据库验证

```bash
sudo mysql -u root -p -e "USE webserver; SELECT * FROM user;"
```

应能看到刚注册的用户（明文密码，仅学习）。

### 6.6 curl 验收

```bash
curl -i -X POST http://127.0.0.1:8080/register -d "username=curl1&passwd=abc"
# 期望：HTTP/1.1 302 Found  Location: /welcome.html

curl -i -X POST http://127.0.0.1:8080/login -d "username=curl1&passwd=abc"
# 期望：302 → /welcome.html
```

### 6.7 成功 checklist

| 检查项 | 预期 |
|--------|------|
| 注册成功 | welcome 页 |
| 重复注册 | register_error |
| 登录成功 | welcome |
| 密码错误 | login_error |
| GET / echo 等 | Step9 能力保留 |
| logs/server.log | 有注册/登录记录 |

---

## 常见问题排查

### Q1：编译找不到 mysql.h

```bash
sudo apt install libmysqlclient-dev
```

### Q2：mysql_real_connect 失败

- MySQL 服务是否启动：`sudo service mysql status`  
- 用户名密码是否正确  
- 是否已 `CREATE DATABASE webserver`  

### Q3：注册总是 register_error

- 用户名是否已存在  
- 查看 `logs/server.log` 里 MySQL 错误信息（最常见：`Unknown column 'passwd'` → 表列名应是 **`password`**，见第二节建表）  
- SQL 里用户名是否含未转义的 `'`  

### Q4：登录永远失败

- `SELECT` 是否查到行  
- 密码是否明文一致（未做哈希）  
- 表单字段名是否为 `username` / `passwd`（HTML `name`）  
- 数据库列名是否为 `password`（与 SQL 一致）  

### Q5：302 后 welcome 404

- `www/welcome.html` 是否存在  
- `pathToFile` 是否映射 `/welcome.html`  

### Q6：连接池耗尽

- 是否每次 `ConnectionRAII` 析构都还连接  
- 增大 `DB_POOL_SIZE` 或检查泄漏  

### Q7：`MYSQL_PORT` / `expected unqualified-id before numeric constant`

- 原因：`#include <mysql/mysql.h>` 后，`MYSQL_PORT` 是**宏**，不能写成 `const int MYSQL_PORT = 3306;`  
- 改法：用本篇的 `DB_HOST`、`DB_PORT`、`DB_USER`、`DB_PASS`、`DB_NAME`、`DB_POOL_SIZE`  

---

## 本篇小结与下一篇预告

### 你现在已经会了什么

- [x] **MySQL 连接池** 取还连接  
- [x] **RAII** 防止连接泄漏  
- [x] 解析 **POST 表单** body  
- [x] **注册 / 登录** 路由 + 302 跳转  
- [x] 单文件 **`Step1to6/step10.cpp`**  

### 路线图

```text
Step9  日志 + eventfd
    ▼
Step10 MySQL + 注册登录  ← 本篇
    ▼
Step11 压力测试 webbench
```

### 下一篇：Step11 压力测试

用 webbench 测 QPS，对比线程数、开/关日志等。详见 [11-压力测试指南](./11-压力测试指南.md)。

### 易错点速查表

| 错误 | 后果 |
|------|------|
| 密码提交 Git | 泄露 |
| 拼接 SQL 不转义 | SQL 注入风险 |
| 忘记 release 连接 | 池耗尽卡死 |
| POST body 未收齐就做登录 | Step6 前的问题，Step10 依赖 Step6 |
| 未链 `-lmysqlclient` | 链接失败 |

---

> **恭喜：** 你的 Web 服务器已经具备 TinyWebServer 简历项目的核心业务能力——**静态站 + 高并发骨架 + 注册登录**。下一步用压测给优化提供数据。
