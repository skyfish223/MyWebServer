#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

const string WEB_ROOT = "www";
const int MAX_EVENTS = 64;
const int PORT = 8080;

struct HttpRequest
{
    string method;
    string path;
    string version;
};

bool parseRequestLine(const string& raw, HttpRequest& req)
{
    size_t lineEnd = raw.find("\r\n");
    if(lineEnd == string::npos) lineEnd = raw.find("\n");
    if(lineEnd == string::npos) return false;
    string line = raw.substr(0, lineEnd);
    istringstream iss(line);
    if(!(iss>>req.method>>req.path>>req.version)) return false;
    return true;
}

string buildHttpResponse(int statusCode, const string& statusText, const string& contentType, const string& body)
{
    return 
        "Http/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "\r\n" + body;
}

string pathToFile(const string& urlPath)
{
    if(urlPath == "/" || urlPath == "/index.html")
        return WEB_ROOT + "/index.html";
    return WEB_ROOT + urlPath;
}

bool isPathSafe(const string& urlPath)
{
    return urlPath.find("..") == string::npos;
}

string getContentType(const string& filePath)
{
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

bool readFile(const string& filePath, string& out)
{
    ifstream ifs(filePath, ios::binary);
    if(!ifs) return false;
    out.assign((istreambuf_iterator<char>(ifs)),istreambuf_iterator<char>());
    return true;
}

string buildResponseFromRequest(const HttpRequest& req)
{
    if(req.method != "GET") return buildHttpResponse(405, "Method Not Allowed",
        "text/plain; charset=utf-8", "暂只支持 GET");
    if(!isPathSafe(req.path)) return buildHttpResponse(403, "Forbidden",
        "text/plain; charset=utf-8", "403 - 非法路径");
    string filePath = pathToFile(req.path);
    string body;
    if(!readFile(filePath, body))
    {
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8", "404 - 文件不存在: " + req.path);
    }
    return buildHttpResponse(200, "OK", getContentType(filePath), body);
}

// Step 4
void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void addFd(int epfd, int fd)
{
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void removeFd(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
}

void handleClient(int epfd, int client_fd)
{
    char buffer[8192] = {0};
    ssize_t n = read(client_fd, buffer, sizeof(buffer)-1);

    string response;
    if(n <= 0)
    {
        response = buildHttpResponse(400, "Bad Request",
            "text/plain; charset=utf-8", "无法读取请求");
    }
    else
    {
        string rawRequest(buffer, n);
        HttpRequest req;
        if(!parseRequestLine(rawRequest, req))
        {
            response = buildHttpResponse(400, "Bad Request",
                "text/plain; charset=utf-8", "请求行格式错误");
        }
        else
        {
            cout << "[fd" << client_fd << "] " << req.method << " " << req.path <<endl;
            response = buildResponseFromRequest(req);
        }
    }
    write(client_fd, response.c_str(), response.size());
    removeFd(epfd, client_fd);
    close(client_fd);
}

int main()
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        cerr << "socket 失败\n";
        return 1;
    }

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
    if(epfd < 0)
    {
        cerr << "epoll_create1 失败\n";
        return 1;
    }

    addFd(epfd, listen_fd);

    cout << "epoll 服务器已启动：http://127.0.0.1:" << PORT << "\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";

    vector<epoll_event> events(MAX_EVENTS);

    while(true)
    {
        int nready = epoll_wait(epfd, events.data(), MAX_EVENTS, -1);
        if(nready < 0)
        {
            cerr << "epoll_wait 失败\n";
            continue;
        }

        for(int i = 0; i < nready; i++)
        {
            int fd = events[i].data.fd;
            if(fd == listen_fd)
            {
                while(true)
                {
                    sockaddr_in client_addr{};
                    socklen_t len =sizeof(client_addr);
                    int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &len);
                    if(client_fd < 0)
                    {
                        if(errno ==EAGAIN || errno == EWOULDBLOCK)
                            break;
                        cerr << "accept 失败\n";
                        break;
                    }
                    setNonBlocking(client_fd);
                    addFd(epfd,client_fd);
                    cout << "新连接 fd=" << client_fd << "\n";
                }
            }
            else
            {
                // 客户端 socket 就绪 = 可以 read
                handleClient(epfd, fd);
            }
        }
    }
    close(epfd);
    close(listen_fd);
    return 0;
}