# 分文件 Step10 —— SqlPool、FormParser、注册登录（详细教程）

> **对应单文件：** [step10.cpp](../../step10.cpp) · [step11.cpp](../../step11.cpp)（拆分前参考）  
> **前置文档：** [09-Step9-Log与eventfd](./09-Step9-Log与eventfd.md) · [10-MySQL登录注册指南](../10-MySQL登录注册指南.md)  
> **本篇目标：** 在 Step9 多文件工程上接入 **MySQL 连接池**；解析 POST 表单；实现 **`/register`、`/login`** 业务；成功/失败用 **HTTP 302** 跳转。  
> **本步新增：** `SqlPool`、`FormParser`；**修改** `ServerConfig`、`HttpResponse`、`HttpHandler`、`main`、`Makefile`

---

## 目录

1. [我们在升级什么](#一我们在升级什么)
2. [本步结束后的文件树](#二本步结束后的文件树)
3. [MySQL 环境与建表](#三mysql-环境与建表)
4. [www 表单页面](#四www-表单页面)
5. [扩展 ServerConfig —— DB_* 常量](#五扩展-serverconfig--db_-常量)
6. [新建 SqlPool](#六新建-serverqlpoolh--serverqlpoolcpp)
7. [新建 FormParser](#七新建-serverformparserh--serverformparsercpp)
8. [扩展 HttpResponse —— 302 与静态页映射](#八扩展-httpresponse--302-与静态页映射)
9. [扩展 HttpHandler —— 注册与登录](#九扩展-httphandler--注册与登录)
10. [修改 main.cpp](#十修改-maincpp)
11. [更新 Makefile](#十一更新-makefile)
12. [编译、运行与验收](#十二编译运行与验收)
13. [常见问题排查](#十三常见问题排查)
14. [本步小结与下一步](#十四本步小结与下一步)

---

## 一、我们在升级什么

### 1.1 Step9 能做什么、还不能做什么

| Step9 已有 | Step10 新增 |
|-----------|------------|
| 异步日志 + `eventfd` 唤醒主线程写响应 | **MySQL 持久化用户** |
| POST `/echo` 回显 body | **POST `/register`、`/login`** |
| GET 静态页 `www/` | 注册/登录成功 **302 跳转** |
| 线程池 `do_request` | **`SqlPool` 连接池** 取还连接 |

真实网站的注册登录链路：

```text
浏览器 GET /register.html     → 展示表单
浏览器 POST /register           → body: username=abc&passwd=123456
  → 状态机收齐 body（Step6）
  → 线程池 do_request
  → SqlPool 取连接 → INSERT
  → 302 Location: /welcome.html

浏览器 GET /log.html            → 登录表单
浏览器 POST /login              → SELECT 比对 password 列
  → 成功 302 /welcome.html
  → 失败 302 /login_error.html
```

### 1.2 生活类比

```text
register.html  = 会员登记表
POST /register = 服务员把表交给后台入库（INSERT）

log.html       = 登录窗口
POST /login    = 查账本有没有这个人、密码对不对（SELECT）

SqlPool        = 前台固定准备 8 条「电话线」连数据库
                 用完还回去，不用每次重新拨号（mysql_real_connect 很贵）
ConnectionRAII = 自动挂机：作用域结束必定 release，防止连接泄漏
```

### 1.3 表单字段名 vs 数据库列名（极易混）

| 位置 | 密码字段名 | 说明 |
|------|-----------|------|
| HTML `<input name="...">` | **`passwd`** | 表单提交名 |
| POST body | **`passwd`** | `username=abc&passwd=123456` |
| C++ 读表单 | `form["passwd"]` | `parse_form_urlencoded` 结果 |
| MySQL 表 `user` | **`password`** | 建表列名 |
| SQL 语句 | **`password`** | `INSERT ... password` / `SELECT password` |

> **千万不要**把数据库列建成 `passwd`，否则 SQL 报 `Unknown column 'passwd'`。

### 1.4 与单文件 step10.cpp 的关系

本篇把 [step10.cpp](../../step10.cpp) 里 **MySQL 相关代码** 拆进 `src/` 目录。  
拆分对照以 [step11.cpp](../../step11.cpp) 为最终参考（Step11 在此基础上加命令行，功能更全）。

### 1.5 安全提醒（必读）

| 学习项目做法 | 生产环境必须 |
|-------------|-------------|
| 密码明文存库 | bcrypt / Argon2 哈希 |
| 字符串拼接 SQL + 简单转义 | **预编译语句**（防 SQL 注入） |
| `DB_PASS` 写在 cpp 常量 | 环境变量 / 配置文件，**不进 Git** |

---

## 二、本步结束后的文件树

在 Step9 基础上 **新增** 两个模块，**修改** 若干已有文件：

```text
MyWebServer/
├── src/
│   ├── main.cpp                 ← 修改：启动时 SqlPool::init
│   ├── ServerConfig.h           ← 修改：声明 DB_* 常量
│   ├── ServerConfig.cpp         ← 修改：定义 DB_* 常量
│   ├── Log.h / Log.cpp          （Step9，本步不动）
│   ├── ThreadPool.h / .cpp      （Step5，本步不动）
│   ├── SqlPool.h                ← 新增
│   ├── SqlPool.cpp              ← 新增
│   ├── FormParser.h             ← 新增
│   ├── FormParser.cpp           ← 新增
│   ├── HttpTypes.h              （本步不动）
│   ├── HttpParser.h / .cpp      （本步不动）
│   ├── HttpResponse.h           ← 修改：buildRedirectResponse
│   ├── HttpResponse.cpp         ← 修改：pathToFile 新页面
│   ├── HttpHandler.h            ← 修改：声明 handle_register/login
│   ├── HttpHandler.cpp          ← 修改：注册登录 + do_request 分支
│   ├── HttpConnection.h / .cpp  （Step9，本步不动）
│   └── EpollHelper.h / .cpp     （本步不动）
├── www/
│   ├── register.html            ← 新增
│   ├── log.html                 ← 新增
│   ├── welcome.html             ← 新增
│   ├── login_error.html         ← 新增
│   └── register_error.html      ← 新增
├── logs/
└── Makefile                     ← 修改：+SqlPool +FormParser +lmysqlclient
```

---

## 三、MySQL 环境与建表

详细安装、排错见 [10-MySQL登录注册指南](../10-MySQL登录注册指南.md)。本节给出最小步骤。

### 3.1 安装（WSL / Ubuntu）

```bash
sudo apt update
sudo apt install mysql-server libmysqlclient-dev
sudo service mysql start
```

### 3.2 建库建表

```bash
sudo mysql -u root -p
```

```sql
CREATE DATABASE IF NOT EXISTS webserver DEFAULT CHARSET utf8mb4;
USE webserver;

CREATE TABLE IF NOT EXISTS user (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(50) NOT NULL
);
```

验证：

```sql
SELECT * FROM user;
```

### 3.3 编译依赖

链接时需要 **`-lmysqlclient`**，头文件 **`#include <mysql/mysql.h>`**。

### 3.4 变量命名：勿用 MYSQL_PORT

`mysql.h` 里已有 `MYSQL_PORT` 等**宏**。若写：

```cpp
const int MYSQL_PORT = 3306;  // 编译报错！
```

会报 `expected unqualified-id before numeric constant`。  
本篇统一用 **`DB_HOST`、`DB_PORT`、`DB_USER`、`DB_PASS`、`DB_NAME`、`DB_POOL_SIZE`**。

---

## 四、www 表单页面

在 `www/` 下创建以下 HTML（完整内容见 [10-MySQL登录注册指南 §3](../10-MySQL登录注册指南.md#第三部分www-表单页面)）。

| 文件 | 作用 |
|------|------|
| `register.html` | 注册表单，`action="/register" method="post"` |
| `log.html` | 登录表单，`action="/login"` |
| `welcome.html` | 成功页 |
| `login_error.html` | 登录失败 |
| `register_error.html` | 注册失败（如用户名重复） |

**注册表单关键字段：**

```html
<input name="username" required maxlength="50">
<input name="passwd" type="password" required maxlength="50">
```

注意密码 input 的 `name` 是 **`passwd`**，不是 `password`。

在 `www/index.html` 增加链接：

```html
<li><a href="/register.html">注册</a></li>
<li><a href="/log.html">登录</a></li>
```

---

## 五、扩展 ServerConfig —— DB_* 常量

数据库连接参数放在 `ServerConfig` 模块，与 `g_cfg` 同级，便于本机修改。

### 5.1 `src/ServerConfig.h` 增加声明

在文件末尾（`g_cfg` 与 `parse_config` 声明之后）追加：

```cpp
// ========== 数据库配置（在 ServerConfig.cpp 定义，本机修改，勿提交 Git）==========
extern const char* DB_HOST;
extern const int   DB_PORT;
extern const char* DB_USER;
extern const char* DB_PASS;
extern const char* DB_NAME;
extern const std::size_t DB_POOL_SIZE;
```

### 5.2 `src/ServerConfig.cpp` 增加定义

```cpp
// ========== 数据库配置（本机修改，勿提交 Git）==========
const char* DB_HOST = "127.0.0.1";
const int   DB_PORT = 3306;
const char* DB_USER = "root";
const char* DB_PASS = "your_password";   // ← 改成你本机 MySQL 密码
const char* DB_NAME = "webserver";
const std::size_t DB_POOL_SIZE = 8;
```

> Step11 会在同一文件加入 `parse_config`；Step10 只需 DB 常量。

---

## 六、新建 `src/SqlPool.h` / `src/SqlPool.cpp`

从 [step10.cpp](../../step10.cpp) 或 [step11.cpp](../../step11.cpp) 迁入 `SqlPool` 与 `ConnectionRAII`。

### 6.1 设计要点

| 方法 | 作用 |
|------|------|
| `init` | 循环 `poolSize` 次 `mysql_real_connect`，全部 push 进队列 |
| `get` | 池空则 `wait`，取队首连接给业务线程 |
| `release` | 用完 push 回队列，`notify_one` 唤醒等待者 |
| `ConnectionRAII` | 构造 `get()`，析构 `release()`，防泄漏 |

### 6.2 `src/SqlPool.h`（完整）

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

### 6.3 `src/SqlPool.cpp`（完整）

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

### 6.4 逐行理解

```text
init：
  for i in 0..poolSize-1:
    conn = mysql_real_connect(...)
    push(conn)
  任一失败 → throw，main 捕获后 return 1

get：
  unique_lock + cv_.wait(池非空)
  pop 队首

release：
  push 回去 + notify_one

ConnectionRAII：
  {
    ConnectionRAII raii;   // 构造：get()
    mysql_query(raii.conn, sql);
  }                        // 析构：release()，即使中间 return 也会还连接
```

---

## 七、新建 `src/FormParser.h` / `src/FormParser.cpp`

POST body 格式：`application/x-www-form-urlencoded`，例如 `username=abc&passwd=123456`。

### 7.1 `src/FormParser.h`（完整）

```cpp
#pragma once

#include <string>
#include <map>

std::string url_decode(const std::string& s);

std::map<std::string, std::string> parse_form_urlencoded(const std::string& body);

std::string escape_sql(const std::string& s);
```

### 7.2 `src/FormParser.cpp`（完整）

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

### 7.3 使用示例

```cpp
auto form = parse_form_urlencoded(req.body);
string user = form["username"];   // 表单字段
string pass = form["passwd"];     // 表单字段 passwd，不是 password
```

---

## 八、扩展 HttpResponse —— 302 与静态页映射

### 8.1 `src/HttpResponse.h` 增加声明

```cpp
std::string buildRedirectResponse(const std::string& location);
```

### 8.2 `src/HttpResponse.cpp` 增加实现

**302 跳转响应：**

```cpp
string buildRedirectResponse(const string& location) {
    return "HTTP/1.1 302 Found\r\n"
           "Location: " + location + "\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n";
}
```

浏览器收到后会自动 GET `Location` 里的路径。

**`pathToFile` 增加映射**（在原有 `/`、`/index.html`、`/echo` 之后追加）：

```cpp
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
```

---

## 九、扩展 HttpHandler —— 注册与登录

### 9.1 `src/HttpHandler.h` 增加声明（可选）

```cpp
std::string handle_register(const HttpRequest& req);
std::string handle_login(const HttpRequest& req);
```

也可只在 `.cpp` 内用 `static`，对外只暴露 `do_request`。

### 9.2 `src/HttpHandler.cpp` 顶部增加 include

```cpp
#include "SqlPool.h"
#include "FormParser.h"
#include "Log.h"
```

### 9.3 `handle_register`（完整）

```cpp
string handle_register(const HttpRequest& req) {
    auto form = parse_form_urlencoded(req.body);
    string user = form["username"];
    string pass = form["passwd"];          // 表单字段 passwd
    if (user.empty() || pass.empty())
        return buildRedirectResponse("/register_error.html");

    ConnectionRAII raii;
    MYSQL* conn = raii.conn;
    // 数据库列名是 password，不是 passwd
    string sql = "INSERT INTO user(username, password) VALUES('"
               + escape_sql(user) + "','" + escape_sql(pass) + "')";
    if (mysql_query(conn, sql.c_str()) != 0) {
        LOG_WARN(string("注册失败: ") + mysql_error(conn));
        return buildRedirectResponse("/register_error.html");
    }
    LOG_INFO("注册成功 user=" + user);
    return buildRedirectResponse("/welcome.html");
}
```

用户名重复时 MySQL 报 UNIQUE 约束错误 → 走 `register_error.html`。

### 9.4 `handle_login`（完整）

```cpp
string handle_login(const HttpRequest& req) {
    auto form = parse_form_urlencoded(req.body);
    string user = form["username"];
    string pass = form["passwd"];          // 表单字段 passwd
    if (user.empty() || pass.empty())
        return buildRedirectResponse("/login_error.html");

    ConnectionRAII raii;
    MYSQL* conn = raii.conn;
    // SELECT 的是表列 password
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
```

### 9.5 扩展 `do_request`

在 POST 分支增加：

```cpp
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

### 9.6 数据流一览

```text
POST /register
  body → parse_form_urlencoded → form["username"], form["passwd"]
  → ConnectionRAII → INSERT ... password ...
  → 302 /welcome.html 或 /register_error.html

POST /login
  body → form["passwd"]
  → SELECT password FROM user WHERE username=...
  → 比对 row[0] 与 pass
  → 302 /welcome.html 或 /login_error.html
```

---

## 十、修改 `main.cpp`

在日志初始化之后、创建线程池之前，初始化连接池：

```cpp
#include "SqlPool.h"
#include "ServerConfig.h"

// ...

try {
    SqlPool::instance().init(DB_HOST, DB_PORT, DB_USER,
                             DB_PASS, DB_NAME, DB_POOL_SIZE);
} catch (const std::exception& e) {
    std::cerr << "MySQL 初始化失败: " << e.what() << "\n";
    return 1;
}
```

启动提示可改为：

```cpp
std::cout << "Step10 已启动 http://127.0.0.1:" << g_cfg.port << "\n";
std::cout << "注册: /register.html  登录: /log.html\n";
```

Step9 的 `eventfd`、`drain_ready_responses`、异步日志 **全部保留**，不要删。

---

## 十一、更新 Makefile

在 `SRCS` 中追加两个新文件，并链接 MySQL 客户端库：

```makefile
SRCS += src/SqlPool.cpp src/FormParser.cpp

LDFLAGS = -lmysqlclient -pthread
```

完整编译命令形态：

```bash
g++ -std=c++17 -Wall -pthread -Isrc -o server $(SRCS) $(LDFLAGS)
```

---

## 十二、编译、运行与验收

### 12.1 准备

```bash
mkdir -p logs www
# 完成第三节 MySQL 建表
# 完成第四节 HTML 页面
# 修改 ServerConfig.cpp 里 DB_PASS
```

### 12.2 编译运行

```bash
cd /path/to/MyWebServer
make clean && make
./server
```

应看到 `MySQL 连接池就绪 size=8`（日志开启时写在 `logs/server.log`）。

### 12.3 浏览器验收（核心）

| 步骤 | 操作 | 预期 |
|------|------|------|
| 1 | 打开 `http://127.0.0.1:8080/register.html` | 注册表单 |
| 2 | 注册 `username=test` / 密码 `123456` | 跳转 **welcome.html** |
| 3 | 打开 `/log.html`，同一账号登录 | 再次进入 welcome |
| 4 | 错误密码登录 | **login_error.html** |
| 5 | 重复注册同一用户名 | **register_error.html** |

### 12.4 curl 验收

```bash
curl -i -X POST http://127.0.0.1:8080/register -d "username=curl1&passwd=abc"
# 期望：HTTP/1.1 302 Found
#       Location: /welcome.html

curl -i -X POST http://127.0.0.1:8080/login -d "username=curl1&passwd=abc"
# 期望：302 → /welcome.html
```

### 12.5 数据库验证

```bash
sudo mysql -u root -p -e "USE webserver; SELECT * FROM user;"
```

应能看到注册用户，`password` 列为明文（仅学习用）。

### 12.6 成功 checklist

| 检查项 | 预期 |
|--------|------|
| 注册成功 | welcome 页 |
| 重复注册 | register_error |
| 登录成功 | welcome |
| 密码错误 | login_error |
| GET `/`、`/echo` | Step9 能力保留 |
| `logs/server.log` | 有注册/登录记录 |
| `make` | 无编译链接错误 |

---

## 十三、常见问题排查

### Q1：编译找不到 `mysql.h`

```bash
sudo apt install libmysqlclient-dev
```

### Q2：`mysql_real_connect` 失败

- MySQL 服务：`sudo service mysql status`
- `DB_USER` / `DB_PASS` 是否正确
- 是否已 `CREATE DATABASE webserver`

### Q3：注册总是 `register_error`

- 用户名是否已存在（UNIQUE）
- 查 `logs/server.log` 中 MySQL 错误
- 最常见：`Unknown column 'passwd'` → 表列名应是 **`password`**
- SQL 中用户名 `'` 是否已 `escape_sql`

### Q4：登录永远失败

- `SELECT` 是否查到行
- 密码明文是否一致（未哈希）
- 表单 `name` 是否为 `username` / **`passwd`**
- SQL 是否 `SELECT password`（列名）

### Q5：302 后 welcome 404

- `www/welcome.html` 是否存在
- `pathToFile` 是否映射 `/welcome.html`

### Q6：连接池耗尽、请求卡死

- 是否每次都用 `ConnectionRAII`（析构必 `release`）
- 可适当增大 `DB_POOL_SIZE`

### Q7：`MYSQL_PORT` 编译报错

改用 `DB_PORT` 等 `DB_*` 命名，见 [第五节](#五扩展-serverconfig--db_-常量)。

---

## 十四、本步小结与下一步

### 14.1 你现在已经会了什么

- [x] **MySQL 连接池** `SqlPool` 取还连接
- [x] **RAII** `ConnectionRAII` 防泄漏
- [x] **表单解析** `parse_form_urlencoded`（`passwd` 表单字段）
- [x] **注册 / 登录** + **302 跳转**（SQL 列 **`password`**）
- [x] 多文件工程拆分 SqlPool / FormParser

### 14.2 模块职责

| 模块 | 职责 |
|------|------|
| `ServerConfig` | `DB_*` 数据库常量 |
| `SqlPool` | 连接池 + RAII |
| `FormParser` | URL 解码、表单解析、SQL 转义 |
| `HttpResponse` | `buildRedirectResponse`、新静态页路径 |
| `HttpHandler` | `handle_register` / `handle_login` |
| `main` | 启动时 `SqlPool::init` |

### 14.3 下一步

**[11-Step11-ServerConfig与Makefile](./11-Step11-ServerConfig与Makefile.md)** —— 命令行 `-p -t -c --no-timer`、关日志压测、**从 step11.cpp 拆出完整 `src/` 工程与 Makefile 终稿**。

---

> **恭喜：** 你的多文件 Web 服务器已具备 TinyWebServer 简历项目的核心业务能力——**静态站 + 高并发骨架 + 注册登录**。下一步用压测与命令行配置把工程打磨成可交付形态。
