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
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

const string WEB_ROOT = "www";
const int MAX_EVENTS = 64;
const int PORT = 8080;
const size_t THREAD_POOL_SIZE = 4;
const size_t MAX_BODY_SIZE = 1024 * 1024;

// Http 状态机
enum class ParseState
{
    RequestLine,
    Header,
    Content
};

enum class ReadResult
{
    Incomplete,
    Complete,
    Error
};

struct HttpRequest
{
    string method;
    string path;
    string version;
    // 因为解析body部分增加的，step 5之前都只解析第一行，所以只需要上面3个
    map<string, string> headers;
    size_t content_length = 0;
    string body;
};

class HttpConnection
{
public:
    explicit HttpConnection(int fd) : fd_(fd) {}

    int fd_;
    ParseState state_ = ParseState::RequestLine;
    string read_buf_;
    HttpRequest request_;
};

static string trim(const string& s)
{
    size_t start = s.find_first_not_of(" \t");
    if(start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}
// 什么是find_first_not_of和last那个
// 为什么只需要找到\n就可以，这样不是计算了\r?
// 意思是(" \t")中的参数是空格 或者 \t这两个字符吗？我一直以为是代表遇到 空格+\n 这一个组合才返回？

static string toLower(string s)
{
    for(char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}// static_cast<char>这个是函数吗？如果是，是不是C++库自己提供的？以及其作用和参数的含义，另外tolower是不是也是c++库提供的函数？

bool get_line(string& buf, string& line)
{
    size_t pos = buf.find("\r\n");
    if(pos == string::npos) return false;
    line = buf.substr(0, pos);
    buf.erase(0, pos+2);
    return true;
}

bool parse_request_line(const string& line, HttpRequest& req)
{
    istringstream iss(line);
    if(!(iss >> req.method >> req.path >> req.version)) return false;
    return true;
}

void parse_header_line(const string& line, HttpRequest& req)
{
    size_t colon = line.find(":");
    if(colon == string::npos) return;
    string key = toLower(trim(line.substr(0, colon)));
    string val = trim(line.substr(colon + 1));
    req.headers[key] = val;
    if(key == "content-length")
    {
        req.content_length = static_cast<size_t>(stoul(val));
    }
}

// read_buf → body（从缓存抠出正文）
ReadResult process_read(HttpConnection& conn)
{
     string line;
     while(true)
     {
        if(conn.state_ == ParseState::Content)
        {
            if(conn.request_.content_length > MAX_BODY_SIZE)
            {
                return ReadResult::Error;
            }
            if(conn.read_buf_.size() >= conn.request_.content_length)
            {
                conn.request_.body = conn.read_buf_.substr(0, conn.request_.content_length);
                conn.read_buf_.erase(0, conn.request_.content_length);
                return ReadResult::Complete;
            }
            return ReadResult::Incomplete;
        }

        if(!get_line(conn.read_buf_, line))
        {
            return ReadResult::Incomplete;
        }

        switch(conn.state_)
        {
            case ParseState::RequestLine:
                if(!parse_request_line(line, conn.request_))
                    return ReadResult::Error;
                conn.state_ = ParseState::Header;
                break;

            case ParseState::Header:
                if(line.empty())
                {
                    if(conn.request_.method == "POST" && conn.request_.content_length > 0)
                    {
                        conn.state_ = ParseState::Content;
                    }
                    else
                    {
                        return ReadResult::Complete;
                    }
                }
                else
                {
                    parse_header_line(line, conn.request_);
                }
                break;

            case ParseState::Content:
                break;
        }
     }
}

// 内核网卡 → read_buf（原始字节入库）
bool append_read(HttpConnection& conn)
{
    char buffer[8192];
    ssize_t n = read(conn.fd_, buffer, sizeof(buffer));
    if(n<0)
    {
        if(errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    if(n==0) return false;
    conn.read_buf_.append(buffer, static_cast<size_t>(n));
    return true;
}

string buildHttpResponse(int statusCode, const string& statusText, const string& contentType, const string& body)
{
    return "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html")
        return WEB_ROOT + "/index.html";
    if (urlPath == "/echo" || urlPath == "/echo.html")
        return WEB_ROOT + "/echo.html";
    return WEB_ROOT + urlPath;
}

bool isPathSafe(const string& urlPath) {
    return urlPath.find("..") == string::npos;
}

string getContentType(const string& filePath) {
    if (filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".html")
        return "text/html; charset=utf-8";
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".css")
        return "text/css; charset=utf-8";
    if (filePath.size() >= 3 && filePath.substr(filePath.size() - 3) == ".js")
        return "application/javascript; charset=utf-8";
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".png")
        return "image/png";
    if (filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".jpg")
        return "image/jpeg";
    return "application/octet-stream";
}

bool readFile(const string& filePath, string& out) {
    ifstream ifs(filePath, ios::binary);
    if (!ifs) return false;
    out.assign((istreambuf_iterator<char>(ifs)),
               istreambuf_iterator<char>());
    return true;
}

string buildGetResponse(const HttpRequest& req) {
    if (!isPathSafe(req.path))
        return buildHttpResponse(403, "Forbidden",
            "text/plain; charset=utf-8", "403 - 非法路径");
    string filePath = pathToFile(req.path);
    string body;
    if (!readFile(filePath, body))
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "404 - 文件不存在: " + req.path);
    return buildHttpResponse(200, "OK", getContentType(filePath), body);
}

string buildPostEchoResponse(const HttpRequest& req)
{
    string msg = "收到 POST body:\n" + req.body;
    return buildHttpResponse(200, "OK", "text/plain; charset=utf-8", msg);
}

string do_request(const HttpRequest& req)
{
    if (req.method == "GET") {
        return buildGetResponse(req);
    }
    if (req.method == "POST") {
        if (req.path == "/echo")
            return buildPostEchoResponse(req);
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "未知 POST 路径: " + req.path);
    }
    return buildHttpResponse(405, "Method Not Allowed",
        "text/plain; charset=utf-8", "暂只支持 GET 和 POST /echo");
}

class ThreadPool 
{
    public:
        explicit ThreadPool(size_t threadCount) : stop_(false) {
            for (size_t i = 0; i < threadCount; ++i) {
                workers_.emplace_back([this]() { workerLoop(); });
            }
        }
    
        ~ThreadPool() {
            {
                lock_guard<mutex> lock(mtx_);
                stop_ = true;
            }
            cv_.notify_all();
            for (auto& t : workers_) {
                if (t.joinable()) t.join();
            }
        }
    
        void submit(function<void()> task) {
            {
                lock_guard<mutex> lock(mtx_);
                tasks_.push(move(task));
            }
            cv_.notify_one();
        }
    
    private:
        void workerLoop() {
            while (true) {
                function<void()> task;
                {
                    unique_lock<mutex> lock(mtx_);
                    cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        }
    
        queue<function<void()>> tasks_;
        mutex mtx_;
        condition_variable cv_;
        vector<thread> workers_;
        bool stop_;
};

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void addFd(int epfd, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void removeFd(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

void closeConnection(int epfd, int fd,
                     unordered_map<int, HttpConnection>& connections) {
    removeFd(epfd, fd);
    close(fd);
    connections.erase(fd);
}

void handle_read(int epfd, HttpConnection& conn, ThreadPool& pool,
    unordered_map<int, HttpConnection>& connections) {
    if (!append_read(conn)) {
    cout << "[main] 读失败或连接关闭 fd=" << conn.fd_ << "\n";
    closeConnection(epfd, conn.fd_, connections);
    return;
    }

    ReadResult result = process_read(conn);

    if (result == ReadResult::Incomplete) {
    return;
    }

    if (result == ReadResult::Error) {
    cout << "[main] 解析错误 fd=" << conn.fd_ << "\n";
    closeConnection(epfd, conn.fd_, connections);
    return;
    }

    HttpRequest req = move(conn.request_);
    int client_fd = conn.fd_;
    removeFd(epfd, client_fd);
    connections.erase(client_fd);

    pool.submit([client_fd, req]() {
    cout << "[worker " << this_thread::get_id() << "] "
    << req.method << " " << req.path;
    if (!req.body.empty()) cout << " body_len=" << req.body.size();
    cout << "\n";

    string response = do_request(req);
    write(client_fd, response.c_str(), response.size());
    close(client_fd);
    cout << "[worker] 完成 fd=" << client_fd << "\n";
    });
}

int main() {
    ThreadPool pool(THREAD_POOL_SIZE);
    unordered_map<int, HttpConnection> connections;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        cerr << "socket 失败\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "bind 失败\n";
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        cerr << "listen 失败\n";
        return 1;
    }

    setNonBlocking(listen_fd);

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        cerr << "epoll_create1 失败\n";
        return 1;
    }

    addFd(epfd, listen_fd);

    cout << "Step6 状态机服务器已启动：http://127.0.0.1:" << PORT << "\n";
    cout << "工作线程数: " << THREAD_POOL_SIZE << "\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";
    cout << "POST 测试: curl -X POST http://127.0.0.1:" << PORT << "/echo -d 'a=b'\n";

    vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        if (nready < 0) {
            cerr << "epoll_wait 失败\n";
            continue;
        }

        for (int i = 0; i < nready; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t len = sizeof(client_addr);
                    int client_fd = accept(listen_fd,
                                           (sockaddr*)&client_addr, &len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        cerr << "accept 失败\n";
                        break;
                    }
                    setNonBlocking(client_fd);
                    addFd(epfd, client_fd);
                    connections.emplace(client_fd, HttpConnection(client_fd));
                    cout << "[main] 新连接 fd=" << client_fd << "\n";
                }
            } else {
                auto it = connections.find(fd);
                if (it == connections.end()) continue;
                handle_read(epfd, it->second, pool, connections);
            }
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}