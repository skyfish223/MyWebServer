#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
// Step 2
#include <string>
#include <sstream>

using namespace std;

// 存放解析的请求信息
struct HttpRequest
{
    string method;
    string path;
    string version;
};

// 解析请求行
bool parseRequestLine(const string& raw, HttpRequest& req)
{
    // 找到第一行末尾
    size_t lineEnd = raw.find("\r\n");
    if(lineEnd == string::npos)
    {
        lineEnd = raw.find('\n');
    }
    if(lineEnd == string::npos)
    {
        return false;
    }

    // 取出第一行
    string line = raw.substr(0, lineEnd);

    // 按照空格分
    istringstream iss(line);
    if(!(iss >> req.method >> req.path >> req.version))
    {
        return false;
    }
    return true;
}

// 正文打包成完整 Http
string buildHttpResponse(int statusCode,
                        const string& statusText,
                        const string& contentType,
                        const string& body)
{
    string response = 
        "Http/1.1 " + to_string(statusCode) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + 
        body;
    return response;
}

// 路由：根据path决定返回什么
string handleRequest(const HttpRequest& req)
{
    // 只支持 GET（浏览器地址栏访问就是 GET）
    if (req.method != "GET")
    {
        return buildHttpResponse(
            405, "Method Not Allowed",
            "text/plain; charset=utf-8",
            "暂只支持 GET 请求，收到的是: " + req.method
        );
    }
    // 路由表
    if(req.path == "/" || req.path == "/index.html")
    {
        string body =
            "<!DOCTYPE html>\r\n"
            "<html><head><meta charset=\"utf-8\"><title>首页</title></head>\r\n"
            "<body>\r\n"
            "<h1>欢迎访问 MyWebServer</h1>\r\n"
            "<p>试试这些链接：</p>\r\n"
            "<ul>\r\n"
            "<li><a href=\"/about\">/about</a> —— 关于页</li>\r\n"
            "<li><a href=\"/hello\">/hello</a> —— 问候页</li>\r\n"
            "<li><a href=\"/not-exist\">/not-exist</a> —— 故意触发 404</li>\r\n"
            "</ul>\r\n"
            "</body></html>\r\n";
        return buildHttpResponse(200, "OK", "text/html; charset=utf-8", body);
    }
    if(req.path == "/about")
    {
        return buildHttpResponse(
            200, "OK", "text/plain; charset=utf-8",
            "这是关于页面。\nMyWebServer 是一个学习用的轻量级 Web 服务器。"
        );
    }
    if(req.path == "/hello")
    {
        return buildHttpResponse(
            200, "OK", "text/plain; charset=utf-8",
            "Hello! 路由生效了，你访问的是 /hello"
        );
    }
    // 没匹配打任何路由
    return buildHttpResponse(
        404, "Not Found", "text/plain; charset=utf-8",
        "404 - 页面不存在: " + req.path
    );
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        cerr<<"socket 创建失败\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    if(bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        cerr<<"bind 失败（端口可能被占用）\n";
        close(server_fd);
        return 1;
    }

    if(listen(server_fd, 10) < 0)
    {
        cerr<<"Listen 失败\n";
        close(server_fd);
        return 1;
    }
    cout<<"服务器已启动：http://127.0.0.1:8080\n";
    cout << "在浏览器打开上述地址，或用 curl 测试\n";

    while(true)
    {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*) &client_addr, &client_len);
        if(client_fd < 0)
        {
            cerr << "accept 失败\n";
            continue;  // 这次失败不退出，继续等下一个
        }

        cout << "收到新连接\n";

        char buffer[4096] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer)-1);

        // Step 2
        string response;
        if (n <= 0) {
            // 没读到有效数据，返回 400
            response = buildHttpResponse(
                400, "Bad Request", "text/plain; charset=utf-8",
                "无法读取请求"
            );
        } else {
            string rawRequest(buffer, n);
            cout << "收到请求（前 120 字符）：\n"
                 << rawRequest.substr(0, min(rawRequest.size(), size_t(120))) << "\n";

            // ========== 【本篇核心】解析 + 路由 ==========
            HttpRequest req;
            if (!parseRequestLine(rawRequest, req)) {
                response = buildHttpResponse(
                    400, "Bad Request", "text/plain; charset=utf-8",
                    "请求行格式错误，期望: GET /path HTTP/1.1"
                );
            } else {
                cout << "解析结果: " << req.method << " " << req.path
                     << " " << req.version << "\n";
                response = handleRequest(req);
            }
        }
        
        write(client_fd, response.c_str(), response.size());
        
        // 4.4 close
        close(client_fd);
        cout << "已响应并关闭连接\n";
    }   

    close(server_fd);
    return 0;
}