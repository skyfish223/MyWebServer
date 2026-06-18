# 分文件 Step11 —— ServerConfig 命令行与完整 Makefile（终稿 / 详细教程）

> **对应单文件：** [step11.cpp](../../step11.cpp) · [11-压力测试指南](../11-压力测试指南.md)  
> **前置文档：** [10-Step10-SqlPool与注册登录](./10-Step10-SqlPool与注册登录.md)  
> **本篇目标：** `-p -t -c --no-timer` 命令行；关日志压测；**从 step11.cpp 一次性拆出完整 `src/` 工程**；根目录 **完整 Makefile**。  
> **本篇特点：** 包含 `src/` 下 **每一个 `.h` / `.cpp` 的完整源码**，与 [step11.cpp](../../step11.cpp) **功能一致**。

---

## 目录

1. [我们在完成什么](#一我们在完成什么)
2. [完整文件树](#二完整文件树)
3. [从 step11.cpp 拆分对照表](#三从-step11cpp-拆分对照表)
4. [ServerConfig —— 命令行与 DB_*](#四serverconfig--命令行与-db_)
5. [Log —— 可关闭的日志宏](#五log--可关闭的日志宏)
6. [ThreadPool](#六threadpool)
7. [SqlPool 与 FormParser](#七sqlpool-与-formparser)
8. [HttpTypes / HttpParser / HttpResponse / HttpHandler](#八httptypes--httpparser--httpresponse--httphandler)
9. [HttpConnection 与 EpollHelper](#九httpconnection-与-epollhelper)
10. [main.cpp](#十maincpp)
11. [完整 Makefile](#十一完整-makefile)
12. [编译、运行与压测](#十二编译运行与压测)
13. [功能回归 checklist](#十三功能回归-checklist)
14. [本系列完结](#十四本系列完结)

---

## 一、我们在完成什么

### 1.1 Step10 → Step11 的增量

| 项目 | Step10 | Step11 |
|------|--------|--------|
| 配置方式 | `ServerConfig` 硬编码默认值 | **`parse_config(argc, argv)`** 命令行 |
| 端口 / 线程数 | 固定 8080 / 4 | **`-p`、`-t`** 运行时指定 |
| 日志 | 始终写 | **`-c 1` 关闭**（压测用） |
| 空闲超时定时器 | 始终扫描 | **`--no-timer` 可关** |
| 工程形态 | 分步累加 | **完整 `src/` + Makefile 终稿** |

### 1.2 命令行用法

```bash
./server -p 8080 -t 4 -c 0          # 日常开发：开日志
./server -p 8080 -t 8 -c 1          # 压测：关日志
./server -p 8080 -t 4 -c 0 --no-timer   # 关空闲连接定时器
./server -h                         # 帮助
```

| 选项 | 含义 |
|------|------|
| `-p <port>` | 监听端口，默认 8080 |
| `-t <threads>` | 线程池大小，默认 4 |
| `-c <0\|1>` | 0=开日志，1=关日志 |
| `--no-timer` | 关闭空闲连接超时扫描 |
| `-h` / `--help` | 打印用法并退出 |

### 1.3 压测为什么要关日志

每条请求写磁盘日志会显著拉低 QPS。`-c 1` 时 `LOG_*` 宏直接空操作，主循环也不再 `Log::init`，用于 webbench 对比。

### 1.4 关键约定（贯穿全文）

- 数据库常量：**`DB_HOST`、`DB_PORT`、`DB_USER`、`DB_PASS`、`DB_NAME`、`DB_POOL_SIZE`**
- 表单密码字段：**`passwd`**（HTML `name`、POST body、`form["passwd"]`）
- 数据库密码列：**`password`**（建表与 SQL）

---

## 二、完整文件树

```text
MyWebServer/
├── Makefile                          ← 本篇 §十一
├── src/
│   ├── main.cpp
│   ├── ServerConfig.h
│   ├── ServerConfig.cpp
│   ├── Log.h
│   ├── Log.cpp
│   ├── ThreadPool.h
│   ├── ThreadPool.cpp
│   ├── SqlPool.h
│   ├── SqlPool.cpp
│   ├── FormParser.h
│   ├── FormParser.cpp
│   ├── HttpTypes.h
│   ├── HttpParser.h
│   ├── HttpParser.cpp
│   ├── HttpResponse.h
│   ├── HttpResponse.cpp
│   ├── HttpHandler.h
│   ├── HttpHandler.cpp
│   ├── HttpConnection.h
│   ├── HttpConnection.cpp
│   ├── EpollHelper.h
│   └── EpollHelper.cpp
├── www/                              （静态页 + 注册登录表单）
├── logs/
└── step11.cpp                        （单文件归档，行为对照）
```

共 **11 对** `.h/.cpp` + **`main.cpp`** + **`Makefile`** = 可交付工程。

---

## 三、从 step11.cpp 拆分对照表

按下列顺序剪切粘贴，**每拆一块 `make` 一次**：

| step11.cpp 区域 | 目标文件 |
|-----------------|----------|
| `struct ServerConfig`、`g_cfg`、`print_usage`、`parse_config`、`DB_*` | `ServerConfig.*` |
| `enum LogLevel`、`class Log` | `Log.*`（宏放 `Log.h`） |
| `class ThreadPool` | `ThreadPool.*` |
| `class SqlPool`、`ConnectionRAII` | `SqlPool.*` |
| `url_decode`、`parse_form_*`、`escape_sql` | `FormParser.*` |
| `HttpRequest`、各类 `enum`、`HttpConnection` 类、`ReadyResponse` | `HttpTypes.h` |
| `trim`、`toLower`、`get_line`、`process_read`、`append_read` | `HttpParser.*` |
| `buildHttp*`、`pathToFile`、`readFile` | `HttpResponse.*` |
| `buildGetResponse`、`handle_register/login`、`do_request` | `HttpHandler.*` |
| `g_ready_queue`、`handle_read/write`、`drain_*`、`process_write` | `HttpConnection.*` |
| `setNonBlocking`、`addFd`、`tick_expired`、`closeConnection` | `EpollHelper.*` |
| `main` | `main.cpp` |

下文给出 **拆分后的完整源码**（直接复制即可）。

---

## 四、ServerConfig —— 命令行与 DB_*

### 4.1 `src/ServerConfig.h`（完整）

```cpp
#pragma once

#include <string>
#include <cstddef>

struct ServerConfig {
    std::string web_root = "www";
    std::string log_path = "logs/server.log";
    int port = 8080;
    std::size_t thread_pool_size = 4;
    int conn_timeout_sec = 15;
    bool log_async = true;
    bool close_log = false;
    bool enable_timer = true;
    std::size_t max_body_size = 1024 * 1024;
    int max_events = 64;
    int listen_backlog = 1024;
};

extern ServerConfig g_cfg;

bool parse_config(int argc, char* argv[]);
void print_usage(const char* prog);

// ========== 数据库配置（ServerConfig.cpp 定义，本机修改，勿提交 Git）==========
extern const char* DB_HOST;
extern const int   DB_PORT;
extern const char* DB_USER;
extern const char* DB_PASS;
extern const char* DB_NAME;
extern const std::size_t DB_POOL_SIZE;
```

### 4.2 `src/ServerConfig.cpp`（完整）

```cpp
#include "ServerConfig.h"

#include <iostream>
#include <string>

ServerConfig g_cfg;

void print_usage(const char* prog) {
    std::cerr << "用法: " << prog << " [选项]\n"
              << "  -p <port>       监听端口，默认 8080\n"
              << "  -t <threads>    线程池大小，默认 4\n"
              << "  -c <0|1>        是否关闭日志：0=开 1=关，默认 0\n"
              << "  --no-timer      关闭空闲连接定时器\n"
              << "  -h              帮助\n"
              << "\n压测示例: " << prog << " -p 8080 -t 8 -c 1\n";
}

bool parse_config(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return false;
        }
        if (arg == "-p" && i + 1 < argc) {
            g_cfg.port = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            g_cfg.thread_pool_size = static_cast<std::size_t>(std::stoi(argv[++i]));
        } else if (arg == "-c" && i + 1 < argc) {
            g_cfg.close_log = (std::stoi(argv[++i]) != 0);
        } else if (arg == "--no-timer") {
            g_cfg.enable_timer = false;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    if (g_cfg.thread_pool_size == 0)
        g_cfg.thread_pool_size = 1;
    return true;
}

// ========== 数据库配置（本机修改，勿提交 Git）==========
const char* DB_HOST = "127.0.0.1";
const int   DB_PORT = 3306;
const char* DB_USER = "root";
const char* DB_PASS = "your_password";
const char* DB_NAME = "webserver";
const std::size_t DB_POOL_SIZE = 8;
```

---

## 五、Log —— 可关闭的日志宏

Step11 的 `LOG_*` 宏会检查 `g_cfg.close_log`，压测时 `-c 1` 不写日志。

### 5.1 `src/Log.h`（完整）

```cpp
#pragma once

#include "ServerConfig.h"

#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>

enum class LogLevel { Debug, Info, Warn, Error };

class Log {
public:
    static Log& instance();

    void init(const std::string& path, bool async);
    void write(LogLevel level, const std::string& msg);

    ~Log();

private:
    Log() = default;

    std::string format_line(LogLevel level, const std::string& msg);
    void write_sync(const std::string& line);
    void write_async(const std::string& line);
    void log_thread_loop();

    std::ofstream file_;
    bool async_ = true;
    bool stop_ = false;
    std::queue<std::string> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
};

#define LOG_INFO(msg)  do { if (!g_cfg.close_log) Log::instance().write(LogLevel::Info,  msg); } while(0)
#define LOG_WARN(msg)  do { if (!g_cfg.close_log) Log::instance().write(LogLevel::Warn,  msg); } while(0)
#define LOG_ERROR(msg) do { if (!g_cfg.close_log) Log::instance().write(LogLevel::Error, msg); } while(0)
```

### 5.2 `src/Log.cpp`（完整）

```cpp
#include "Log.h"

#include <ctime>
#include <iostream>

using namespace std;

Log& Log::instance() {
    static Log inst;
    return inst;
}

void Log::init(const string& path, bool async) {
    async_ = async;
    file_.open(path, ios::app);
    if (!file_) {
        cerr << "无法打开日志: " << path << "\n";
        return;
    }
    if (async_) {
        stop_ = false;
        worker_ = thread([this]() { log_thread_loop(); });
    }
}

void Log::write(LogLevel level, const string& msg) {
    string line = format_line(level, msg);
    if (async_)
        write_async(line);
    else
        write_sync(line);
}

Log::~Log() {
    if (async_ && worker_.joinable()) {
        {
            lock_guard<mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    if (file_.is_open())
        file_.close();
}

string Log::format_line(LogLevel level, const string& msg) {
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

void Log::write_sync(const string& line) {
    lock_guard<mutex> lock(mtx_);
    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
}

void Log::write_async(const string& line) {
    {
        lock_guard<mutex> lock(mtx_);
        queue_.push(line);
    }
    cv_.notify_one();
}

void Log::log_thread_loop() {
    while (true) {
        string line;
        {
            unique_lock<mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
                return;
            line = move(queue_.front());
            queue_.pop();
        }
        if (file_.is_open()) {
            file_ << line << '\n';
            file_.flush();
        }
    }
}
```

---

## 六、ThreadPool

### 6.1 `src/ThreadPool.h`（完整）

```cpp
#pragma once

#include <functional>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstddef>

class ThreadPool {
public:
    explicit ThreadPool(std::size_t n);
    ~ThreadPool();

    void submit(std::function<void()> task);

private:
    void workerLoop();

    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};
```

### 6.2 `src/ThreadPool.cpp`（完整）

```cpp
#include "ThreadPool.h"

using namespace std;

ThreadPool::ThreadPool(size_t n) : stop_(false) {
    for (size_t i = 0; i < n; ++i)
        workers_.emplace_back([this]() { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        lock_guard<mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
}

void ThreadPool::submit(function<void()> task) {
    {
        lock_guard<mutex> lk(mtx_);
        tasks_.push(move(task));
    }
    cv_.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        function<void()> task;
        {
            unique_lock<mutex> lk(mtx_);
            cv_.wait(lk, [this]() { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
                return;
            task = move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}
```

---

## 七、SqlPool 与 FormParser

与 [Step10 教程](./10-Step10-SqlPool与注册登录.md) 相同，此处给出终稿完整源码。

### 7.1 `src/SqlPool.h`（完整）

```cpp
#pragma once

#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstddef>

class SqlPool {
public:
    static SqlPool& instance();

    void init(const char* host, int port, const char* user,
              const char* pass, const char* db, std::size_t poolSize);

    MYSQL* get();
    void release(MYSQL* conn);

private:
    SqlPool() = default;

    MYSQL* create_connection(const char* host, int port,
                             const char* user, const char* pass, const char* db);

    std::queue<MYSQL*> pool_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

struct ConnectionRAII {
    MYSQL* conn;
    ConnectionRAII();
    ~ConnectionRAII();
};
```

### 7.2 `src/SqlPool.cpp`（完整）

```cpp
#include "SqlPool.h"
#include "Log.h"

#include <stdexcept>
#include <string>

using namespace std;

SqlPool& SqlPool::instance() {
    static SqlPool p;
    return p;
}

void SqlPool::init(const char* host, int port, const char* user,
                   const char* pass, const char* db, size_t poolSize) {
    for (size_t i = 0; i < poolSize; ++i) {
        MYSQL* conn = create_connection(host, port, user, pass, db);
        if (!conn)
            throw runtime_error("MySQL 连接池初始化失败");
        pool_.push(conn);
    }
    LOG_INFO("MySQL 连接池就绪 size=" + to_string(poolSize));
}

MYSQL* SqlPool::get() {
    unique_lock<mutex> lock(mtx_);
    cv_.wait(lock, [this]() { return !pool_.empty(); });
    MYSQL* conn = pool_.front();
    pool_.pop();
    return conn;
}

void SqlPool::release(MYSQL* conn) {
    if (!conn) return;
    lock_guard<mutex> lock(mtx_);
    pool_.push(conn);
    cv_.notify_one();
}

MYSQL* SqlPool::create_connection(const char* host, int port,
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

ConnectionRAII::ConnectionRAII() : conn(SqlPool::instance().get()) {}

ConnectionRAII::~ConnectionRAII() {
    SqlPool::instance().release(conn);
}
```

### 7.3 `src/FormParser.h`（完整）

```cpp
#pragma once

#include <string>
#include <map>

std::string url_decode(const std::string& s);

std::map<std::string, std::string> parse_form_urlencoded(const std::string& body);

std::string escape_sql(const std::string& s);
```

### 7.4 `src/FormParser.cpp`（完整）

```cpp
#include "FormParser.h"

using namespace std;

string url_decode(const string& s) {
    string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = s[i + 1], lo = s[i + 2];
            auto hex = [](int c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int h = hex(hi), l = hex(lo);
            if (h >= 0 && l >= 0) {
                out += static_cast<char>((h << 4) | l);
                i += 2;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

map<string, string> parse_form_urlencoded(const string& body) {
    map<string, string> m;
    size_t start = 0;
    while (start < body.size()) {
        size_t amp = body.find('&', start);
        string pair = body.substr(start,
            amp == string::npos ? string::npos : amp - start);
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
```

---

## 八、HttpTypes / HttpParser / HttpResponse / HttpHandler

### 8.1 `src/HttpTypes.h`（完整）

```cpp
#pragma once

#include <string>
#include <map>
#include <ctime>

enum class ParseState { RequestLine, Header, Content };
enum class ReadResult { Incomplete, Complete, Error };
enum class WriteResult { Incomplete, Complete, Error };
enum class ConnPhase { Reading, WaitingResponse, Writing };

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string body;
    std::map<std::string, std::string> headers;
    std::size_t content_length = 0;
};

class HttpConnection {
public:
    explicit HttpConnection(int fd);

    int fd_;
    ParseState state_ = ParseState::RequestLine;
    std::string read_buf_;
    HttpRequest request_;
    time_t last_active_;
    ConnPhase phase_ = ConnPhase::Reading;
    std::string write_buf_;
    std::size_t write_idx_ = 0;

    void refresh_active();
    bool is_expired(int timeout_sec) const;
};

struct ReadyResponse {
    int fd;
    std::string data;
};
```

### 8.2 `src/HttpParser.h`（完整）

```cpp
#pragma once

#include "HttpTypes.h"

bool get_line(std::string& buf, std::string& line);

bool parse_request_line(const std::string& line, HttpRequest& req);

void parse_header_line(const std::string& line, HttpRequest& req);

ReadResult process_read(HttpConnection& conn);

bool append_read(HttpConnection& conn);
```

### 8.3 `src/HttpParser.cpp`（完整）

```cpp
#include "HttpParser.h"
#include "ServerConfig.h"

#include <sstream>
#include <cctype>
#include <unistd.h>
#include <cerrno>

using namespace std;

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

static string toLower(string s) {
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool get_line(string& buf, string& line) {
    size_t p = buf.find("\r\n");
    if (p == string::npos) return false;
    line = buf.substr(0, p);
    buf.erase(0, p + 2);
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
    if (k == "content-length")
        req.content_length = static_cast<size_t>(stoul(v));
}

ReadResult process_read(HttpConnection& conn) {
    string line;
    while (true) {
        if (conn.state_ == ParseState::Content) {
            if (conn.request_.content_length > g_cfg.max_body_size)
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
                if (conn.request_.method == "POST" && conn.request_.content_length > 0)
                    conn.state_ = ParseState::Content;
                else
                    return ReadResult::Complete;
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
    char buf[8192];
    ssize_t n = read(conn.fd_, buf, sizeof(buf));
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK);
    if (n == 0)
        return false;
    conn.read_buf_.append(buf, static_cast<size_t>(n));
    return true;
}
```

### 8.4 `src/HttpResponse.h`（完整）

```cpp
#pragma once

#include <string>

std::string buildHttpResponse(int code,
                              const std::string& text,
                              const std::string& type,
                              const std::string& body);

std::string buildRedirectResponse(const std::string& location);

std::string pathToFile(const std::string& urlPath);

bool isPathSafe(const std::string& path);

std::string getContentType(const std::string& filePath);

bool readFile(const std::string& filePath, std::string& out);
```

### 8.5 `src/HttpResponse.cpp`（完整）

```cpp
#include "HttpResponse.h"
#include "ServerConfig.h"

#include <fstream>
#include <iterator>

using namespace std;

string buildHttpResponse(int code, const string& text, const string& type, const string& body) {
    return "HTTP/1.1 " + to_string(code) + " " + text + "\r\n"
           "Content-Type: " + type + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n\r\n" + body;
}

string buildRedirectResponse(const string& location) {
    return "HTTP/1.1 302 Found\r\n"
           "Location: " + location + "\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n";
}

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html")
        return g_cfg.web_root + "/index.html";
    if (urlPath == "/echo" || urlPath == "/echo.html")
        return g_cfg.web_root + "/echo.html";
    if (urlPath == "/log" || urlPath == "/log.html")
        return g_cfg.web_root + "/log.html";
    if (urlPath == "/register" || urlPath == "/register.html")
        return g_cfg.web_root + "/register.html";
    if (urlPath == "/welcome" || urlPath == "/welcome.html")
        return g_cfg.web_root + "/welcome.html";
    if (urlPath == "/login_error.html")
        return g_cfg.web_root + "/login_error.html";
    if (urlPath == "/register_error.html")
        return g_cfg.web_root + "/register_error.html";
    return g_cfg.web_root + urlPath;
}

bool isPathSafe(const string& p) {
    return p.find("..") == string::npos;
}

string getContentType(const string& fp) {
    if (fp.size() >= 5 && fp.substr(fp.size() - 5) == ".html")
        return "text/html; charset=utf-8";
    if (fp.size() >= 4 && fp.substr(fp.size() - 4) == ".css")
        return "text/css; charset=utf-8";
    return "application/octet-stream";
}

bool readFile(const string& fp, string& out) {
    ifstream ifs(fp, ios::binary);
    if (!ifs) return false;
    out.assign(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
    return true;
}
```

### 8.6 `src/HttpHandler.h`（完整）

```cpp
#pragma once

#include "HttpTypes.h"
#include <string>

std::string do_request(const HttpRequest& req);
```

### 8.7 `src/HttpHandler.cpp`（完整）

```cpp
#include "HttpHandler.h"
#include "HttpResponse.h"
#include "FormParser.h"
#include "SqlPool.h"
#include "Log.h"

#include <mysql/mysql.h>

using namespace std;

static string buildGetResponse(const HttpRequest& req) {
    if (!isPathSafe(req.path))
        return buildHttpResponse(403, "Forbidden", "text/plain; charset=utf-8", "403");
    string fp = pathToFile(req.path), body;
    if (!readFile(fp, body))
        return buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8", "404: " + req.path);
    return buildHttpResponse(200, "OK", getContentType(fp), body);
}

static string handle_register(const HttpRequest& req) {
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

static string handle_login(const HttpRequest& req) {
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

static string buildPostEchoResponse(const HttpRequest& req) {
    return buildHttpResponse(200, "OK", "text/plain; charset=utf-8",
        "收到 POST body:\n" + req.body);
}

string do_request(const HttpRequest& req) {
    if (req.method == "GET")
        return buildGetResponse(req);
    if (req.method == "POST") {
        if (req.path == "/echo")
            return buildPostEchoResponse(req);
        if (req.path == "/register")
            return handle_register(req);
        if (req.path == "/login")
            return handle_login(req);
        return buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8",
            "未知 POST: " + req.path);
    }
    return buildHttpResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "405");
}
```

---

## 九、HttpConnection 与 EpollHelper

### 9.1 `src/HttpConnection.h`（完整）

```cpp
#pragma once

#include "HttpTypes.h"
#include "ThreadPool.h"

#include <unordered_map>
#include <queue>
#include <mutex>

extern std::queue<ReadyResponse> g_ready_queue;
extern std::mutex g_ready_mtx;
extern int g_notify_fd;

void wake_main_thread();

void drain_ready_responses(int epfd, std::unordered_map<int, HttpConnection>& conns);

WriteResult process_write(int epfd, HttpConnection& conn);

void handle_write(int epfd, HttpConnection& conn,
                  std::unordered_map<int, HttpConnection>& conns);

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
                 std::unordered_map<int, HttpConnection>& conns);
```

### 9.2 `src/HttpConnection.cpp`（完整）

```cpp
#include "HttpConnection.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "EpollHelper.h"
#include "ServerConfig.h"
#include "Log.h"

#include <vector>
#include <unistd.h>
#include <cerrno>
#include <sys/epoll.h>

using namespace std;

queue<ReadyResponse> g_ready_queue;
mutex g_ready_mtx;
int g_notify_fd = -1;

HttpConnection::HttpConnection(int fd) : fd_(fd), last_active_(time(nullptr)) {}

void HttpConnection::refresh_active() {
    last_active_ = time(nullptr);
}

bool HttpConnection::is_expired(int timeout_sec) const {
    return difftime(time(nullptr), last_active_) >= timeout_sec;
}

void wake_main_thread() {
    if (g_notify_fd < 0) return;
    uint64_t one = 1;
    write(g_notify_fd, &one, sizeof(one));
}

WriteResult process_write(int epfd, HttpConnection& c) {
    c.refresh_active();
    while (c.write_idx_ < c.write_buf_.size()) {
        ssize_t n = write(c.fd_, c.write_buf_.data() + c.write_idx_,
                          c.write_buf_.size() - c.write_idx_);
        if (n > 0) {
            c.write_idx_ += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            modFd(epfd, c.fd_, EPOLLOUT);
            return WriteResult::Incomplete;
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
    {
        lock_guard<mutex> lk(g_ready_mtx);
        swap(local, g_ready_queue);
    }
    while (!local.empty()) {
        auto rr = move(local.front());
        local.pop();
        auto it = conns.find(rr.fd);
        if (it == conns.end()) continue;
        HttpConnection& c = it->second;
        if (c.phase_ != ConnPhase::WaitingResponse) continue;
        c.write_buf_ = move(rr.data);
        c.write_idx_ = 0;
        c.phase_ = ConnPhase::Writing;
        modFd(epfd, c.fd_, EPOLLOUT);
        WriteResult w = process_write(epfd, c);
        if (w == WriteResult::Complete || w == WriteResult::Error)
            closeConnection(epfd, c.fd_, conns);
    }
}

void handle_read(int epfd, HttpConnection& c, ThreadPool& pool,
                 unordered_map<int, HttpConnection>& conns) {
    if (c.phase_ != ConnPhase::Reading) return;
    if (!append_read(c)) {
        closeConnection(epfd, c.fd_, conns);
        return;
    }
    c.refresh_active();
    ReadResult r = process_read(c);
    if (r == ReadResult::Incomplete) return;
    if (r == ReadResult::Error) {
        closeConnection(epfd, c.fd_, conns);
        return;
    }
    HttpRequest req = move(c.request_);
    int fd = c.fd_;
    c.phase_ = ConnPhase::WaitingResponse;
    modFd(epfd, fd, 0);
    if (!g_cfg.close_log)
        LOG_INFO("请求完整 " + req.method + " " + req.path);
    pool.submit([fd, req]() {
        string resp = do_request(req);
        {
            lock_guard<mutex> lk(g_ready_mtx);
            g_ready_queue.push({fd, move(resp)});
        }
        wake_main_thread();
    });
}
```

### 9.3 `src/EpollHelper.h`（完整）

```cpp
#pragma once

#include "HttpTypes.h"

#include <cstdint>
#include <unordered_map>

void setNonBlocking(int fd);

void addFd(int epfd, int fd, uint32_t events = 0x001);

void modFd(int epfd, int fd, uint32_t events);

void removeFd(int epfd, int fd);

void closeConnection(int epfd, int fd, std::unordered_map<int, HttpConnection>& conns);

void tick_expired_connections(int epfd, std::unordered_map<int, HttpConnection>& conns);
```

### 9.4 `src/EpollHelper.cpp`（完整）

```cpp
#include "EpollHelper.h"
#include "ServerConfig.h"
#include "Log.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <vector>
#include <string>

using namespace std;

void setNonBlocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void addFd(int epfd, int fd, uint32_t ev) {
    epoll_event e{};
    e.events = ev;
    e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e);
}

void modFd(int epfd, int fd, uint32_t ev) {
    epoll_event e{};
    e.events = ev;
    e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
}

void removeFd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

void closeConnection(int epfd, int fd, unordered_map<int, HttpConnection>& conns) {
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    removeFd(epfd, fd);
    close(fd);
    conns.erase(it);
}

void tick_expired_connections(int epfd, unordered_map<int, HttpConnection>& conns) {
    vector<int> v;
    for (auto& [fd, c] : conns) {
        if (c.phase_ == ConnPhase::Reading && c.is_expired(g_cfg.conn_timeout_sec))
            v.push_back(fd);
    }
    for (int fd : v) {
        LOG_WARN("超时 fd=" + to_string(fd));
        closeConnection(epfd, fd, conns);
    }
}
```

---

## 十、main.cpp

### `src/main.cpp`（完整）

```cpp
#include "ServerConfig.h"
#include "Log.h"
#include "ThreadPool.h"
#include "SqlPool.h"
#include "HttpConnection.h"
#include "EpollHelper.h"

#include <iostream>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>

using namespace std;

int main(int argc, char* argv[]) {
    if (!parse_config(argc, argv))
        return 0;

    if (!g_cfg.close_log)
        Log::instance().init(g_cfg.log_path, g_cfg.log_async);

    try {
        SqlPool::instance().init(DB_HOST, DB_PORT, DB_USER,
                                 DB_PASS, DB_NAME, DB_POOL_SIZE);
    } catch (const exception& e) {
        cerr << "MySQL 初始化失败: " << e.what() << "\n";
        return 1;
    }

    ThreadPool pool(g_cfg.thread_pool_size);
    unordered_map<int, HttpConnection> connections;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return 1;
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_cfg.port);
    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, g_cfg.listen_backlog);
    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);
    addFd(epfd, listen_fd);
    g_notify_fd = eventfd(0, EFD_NONBLOCK);
    addFd(epfd, g_notify_fd);

    cout << "Step11 已启动 http://127.0.0.1:" << g_cfg.port << "\n";
    cout << "线程=" << g_cfg.thread_pool_size
         << " 日志=" << (g_cfg.close_log ? "关" : "开")
         << " 定时器=" << (g_cfg.enable_timer ? "开" : "关") << "\n";
    if (!g_cfg.close_log)
        LOG_INFO("Step11 服务器启动");

    vector<epoll_event> events(g_cfg.max_events);
    while (true) {
        int n = epoll_wait(epfd, events.data(), g_cfg.max_events, -1);
        if (n < 0) continue;
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (fd == g_notify_fd) {
                uint64_t u;
                while (read(g_notify_fd, &u, sizeof(u)) > 0) {}
                drain_ready_responses(epfd, connections);
                continue;
            }
            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in ca{};
                    socklen_t len = sizeof(ca);
                    int cfd = accept(listen_fd, (sockaddr*)&ca, &len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        break;
                    }
                    setNonBlocking(cfd);
                    addFd(epfd, cfd);
                    connections.emplace(cfd, HttpConnection(cfd));
                }
            } else {
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                if (ev & EPOLLIN)
                    handle_read(epfd, it->second, pool, connections);
                if (ev & EPOLLOUT) {
                    it = connections.find(fd);
                    if (it != connections.end())
                        handle_write(epfd, it->second, connections);
                }
            }
        }
        drain_ready_responses(epfd, connections);
        if (g_cfg.enable_timer)
            tick_expired_connections(epfd, connections);
    }
    return 0;
}
```

---

## 十一、完整 Makefile

在项目根目录创建 **`Makefile`**（完整）：

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -Isrc
LDFLAGS  = -lmysqlclient -pthread

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
       src/Log.cpp \
       src/ThreadPool.cpp \
       src/SqlPool.cpp \
       src/FormParser.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp \
       src/HttpConnection.cpp \
       src/EpollHelper.cpp

server: $(SRCS)
	$(CXX) $(CXXFLAGS) -o server $(SRCS) $(LDFLAGS)

clean:
	rm -f server

.PHONY: clean server
```

### 编译说明

| 项 | 值 |
|----|-----|
| `-Isrc` | 头文件在 `src/` 下，统一 `#include "Xxx.h"` |
| `-lmysqlclient` | 链接 MySQL C API |
| `-pthread` | 线程池、异步日志 |

---

## 十二、编译、运行与压测

### 12.1 一次性搭建

```bash
cd /path/to/MyWebServer

# 1. 按本篇创建 src/ 下全部文件
# 2. 创建根目录 Makefile
# 3. 修改 ServerConfig.cpp 中 DB_PASS
# 4. 确保 MySQL 已建库建表（见 Step10 教程）
# 5. 确保 www/ 静态页齐全

mkdir -p logs
make clean && make
```

### 12.2 日常开发

```bash
./server -p 8080 -t 4 -c 0
curl http://127.0.0.1:8080/
cat logs/server.log
```

### 12.3 压测（关日志）

```bash
ulimit -n 65535
./server -p 8080 -t 8 -c 1

./test_pressure/webbench-1.5/webbench -c 500 -t 5 http://127.0.0.1:8080/index.html
```

压测细节见 [11-压力测试指南](../11-压力测试指南.md) 与 [test_pressure/README.md](../../test_pressure/README.md)。

### 12.4 命令行组合示例

```bash
# 8 线程、关日志、关定时器（部分压测场景）
./server -p 8080 -t 8 -c 1 --no-timer

# 单线程对比实验
./server -p 8080 -t 1 -c 1
./server -p 8080 -t 4 -c 1
./server -p 8080 -t 8 -c 1
```

---

## 十三、功能回归 checklist

- [ ] `curl http://127.0.0.1:8080/` 返回首页
- [ ] `curl -X POST http://127.0.0.1:8080/echo -d "a=b"` 回显 body
- [ ] 浏览器注册 → `welcome.html`
- [ ] 浏览器登录 / 错误密码 → 对应页面
- [ ] `-c 0` 时 `logs/server.log` 有启动与请求记录
- [ ] `-c 1` 压测 `Failed requests: 0`（webbench）
- [ ] `-h` 打印用法并正常退出
- [ ] 多文件 `server` 与单文件 `step11.cpp` **行为一致**

---

## 十四、本系列完结

### 14.1 阶段产出

| 阶段 | 产出 |
|------|------|
| Step1～11 分文件教程 | 完整 `src/` 工程 |
| 单文件归档 | `Step1to6/`、`step10.cpp`、`step11.cpp` |
| 压测 | `test_pressure/` |

### 14.2 模块总览

| 模块 | 职责 |
|------|------|
| `ServerConfig` | 全局配置、`parse_config`、`DB_*` |
| `Log` | 异步日志，`-c 1` 可关 |
| `ThreadPool` | 工作线程执行 `do_request` |
| `SqlPool` | MySQL 连接池 + RAII |
| `FormParser` | 表单解析、SQL 转义 |
| `HttpParser` | 三态状态机收请求 |
| `HttpResponse` | 静态文件、302 |
| `HttpHandler` | 路由与注册登录 |
| `HttpConnection` | 读写阶段、eventfd 队列 |
| `EpollHelper` | epoll 封装、超时踢连接 |
| `main` | 组装启动与主循环 |

### 14.3 可选后续

- **Step12**：ET 模式、Keep-Alive（在 `EpollHelper` / `HttpConnection` 上扩展）
- 对照 [TinyWebServer](https://github.com/qinguoyi/TinyWebServer) 精修细节
- 密码哈希、预编译语句（生产化）

---

> **恭喜：** 你已完成从单文件学习到多文件交付的完整迁移路线。`src/` 工程与 `step11.cpp` 功能一致，可直接用于简历项目展示与 webbench 压测优化。
