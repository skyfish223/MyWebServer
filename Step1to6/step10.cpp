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
// 勿用 MYSQL_PORT 等名：mysql.h 里已有同名宏
const char* DB_HOST = "127.0.0.1";
const int   DB_PORT = 3306;
const char* DB_USER = "root";
const char* DB_PASS = "123456";
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

class SqlPool   
{
public:
    static SqlPool& instance() { static SqlPool p; return p; }

    void init(const char* host, int port, const char* user,
                const char* pass, const char* db, size_t poolSize)
    {
        for(size_t i = 0; i < poolSize; ++i)
        {
            MYSQL* conn = create_connection(host, port, user, pass, db);
            if(!conn) throw runtime_error("MySQL 连接池初始化失败");
            pool_.push(conn);
        }
        LOG_INFO("MySQL 连接池就绪 size=" + to_string(poolSize));
    }

    MYSQL* get()
    {
        unique_lock<mutex> lock(mtx_);
        cv_.wait(lock, [this] () { return !pool_.empty(); });
        MYSQL* conn = pool_.front();
        pool_.pop();
        return conn;
    }

    void release(MYSQL* conn)
    {
        if(!conn) return;
        lock_guard<mutex> lock(mtx_);
        pool_.push(conn);
        cv_.notify_one();
    }

private:
    SqlPool() = default;

    MYSQL* create_connection(const char* host, int port, const char* user, const char* pass, const char* db)
    {
        MYSQL* conn = mysql_init(nullptr);
        if(!conn) return nullptr;
        if(!mysql_real_connect(conn, host, user, pass, db, port, nullptr, 0))
        {
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

struct ConnectionRAII
{
    MYSQL* conn;
    ConnectionRAII() : conn(SqlPool::instance().get()) {}
    ~ConnectionRAII() { SqlPool::instance().release(conn); }
};

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
}//辅助函数，用于生成 HTTP 302 重定向响应。它的作用是告诉浏览器：“你请求的资源暂时移到了另一个 URL（即 Location 头指定的地址），请自动去那里重新发起 GET 请求。”

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