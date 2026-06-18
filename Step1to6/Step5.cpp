#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
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

struct HttpRequest {
    string method;
    string path;
    string version;
};

bool parseRequestLine(const string& raw, HttpRequest& req) {
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == string::npos) lineEnd = raw.find('\n');
    if (lineEnd == string::npos) return false;
    string line = raw.substr(0, lineEnd);
    istringstream iss(line);
    if (!(iss >> req.method >> req.path >> req.version)) return false;
    return true;
}

string buildHttpResponse(int statusCode, const string& statusText,
                         const string& contentType, const string& body) {
    return "HTTP/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
           "Content-Type: " + contentType + "\r\n"
           "Content-Length: " + to_string(body.size()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

string pathToFile(const string& urlPath) {
    if (urlPath == "/" || urlPath == "/index.html")
        return WEB_ROOT + "/index.html";
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

string buildResponseFromRequest(const HttpRequest& req) {
    if (req.method != "GET")
        return buildHttpResponse(405, "Method Not Allowed",
            "text/plain; charset=utf-8", "暂只支持 GET");
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

string processRawRequest(const string& rawRequest) {
    HttpRequest req;
    if (!parseRequestLine(rawRequest, req))
        return buildHttpResponse(400, "Bad Request",
            "text/plain; charset=utf-8", "请求行格式错误");
    cout << "[worker " << this_thread::get_id() << "] "
         << req.method << " " << req.path << "\n";
    return buildResponseFromRequest(req);
}

class ThreadPool {
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

void dispatchClient(int epfd, int client_fd, ThreadPool& pool) {
    char buffer[8192] = {0};
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    removeFd(epfd, client_fd);

    string rawRequest;
    if (n > 0) rawRequest.assign(buffer, n);

    pool.submit([client_fd, rawRequest, n]() {
        string response;
        if (n <= 0) {
            response = buildHttpResponse(400, "Bad Request",
                "text/plain; charset=utf-8", "无法读取请求");
        } else {
            response = processRawRequest(rawRequest);
        }
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "[worker] 完成 fd=" << client_fd << "\n";
    });
}

int main() {
    ThreadPool pool(THREAD_POOL_SIZE);

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

    cout << "线程池服务器已启动：http://127.0.0.1:" << PORT << "\n";
    cout << "工作线程数: " << THREAD_POOL_SIZE << "\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";

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
                    cout << "[main] 新连接 fd=" << client_fd << "\n";
                }
            } else {
                dispatchClient(epfd, fd, pool);
            }
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
