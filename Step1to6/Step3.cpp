#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
// Step 3
#include <fstream>

using namespace std;

// 根目录
const string WEB_ROOT = "www";

struct HttpRequest {
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
    if(!(iss >> req.method >> req.path >> req.version)) return false;
    return true;
}

string buildHttpResponse(int statusCode,
                        const string& statusText,
                        const string& contentType,
                        const string& body
)
{
    return "Http/1.1" + to_string(statusCode) + " " + statusText + "\r\n"
            "Content-type: " + contentType + "\r\n"
            "Content-Length: " + to_string(body.size()) + "\r\n"
            "\r\n" + body;
}
// Step 3 add URL
string pathToFile(const string& urlPath)
{
    if(urlPath == "/" || urlPath == "/index.html")
    {
        return WEB_ROOT + "/index.html";
    }
    return WEB_ROOT + urlPath;
}

// Step 3 check safety
bool isPathSafe(const string& urlPath)
{
    if(urlPath.find("..") != string::npos) return false;
    return true;
}

// Step 3 select the contetn-type
string getContentType(const string& filePath)
{
    if(filePath.size() >= 5 && filePath.substr(filePath.size()-5) == ".html")
        return "text/html; charset=utf-8";
    if(filePath.size() >= 4 && filePath.substr(filePath.size()-4) == ".css")
        return "text/css; charset=utf-8";
    if(filePath.size() >= 3 && filePath.substr(filePath.size()-3) == ".js")
        return "application/javascript; charset=utf-8";
    if(filePath.size() >= 4 && filePath.substr(filePath.size()-4) == ".png")
        return "image/png";
    if(filePath.size() >= 4 && filePath.substr(filePath.size()-4) == ".jpg")
        return "image/jpeg";
    if(filePath.size() >= 5 && filePath.substr(filePath.size()-5) == ".jpeg")
        return "image/jpeg";
    return "application/octet-stream";  // 未知类型：二进制流
}

// Step 3 read file
bool readFile(const string& filePath, string& out)
{
    ifstream ifs(filePath, ios::binary);
    if(!ifs)
    {
        return false;
    }
    out.assign((istreambuf_iterator<char>(ifs)),istreambuf_iterator<char>());
    return true;
}

// Update handleRequest
string handleRequest(const HttpRequest& req)
{
    if(req.method != "GET")
    {
        return buildHttpResponse(
            405,"Method Not Allowed","text/plain; charset=utf-8", "暂只支持 GET，收到:" + req.method
        );
    }

    if(!isPathSafe(req.path))
    {
        return buildHttpResponse(
            403,"Forbidden","text/plain; charset=utf-8","403 - 非法路径"
        );
    }
    
    string filePath = pathToFile(req.path);
    string body;
    if(!readFile(filePath,body))
    {
        return buildHttpResponse(404, "Not Found",
            "text/plain; charset=utf-8",
            "404 - 文件不存在: " + req.path);
    }

    string contentType = getContentType(filePath);
    return buildHttpResponse(200, "OK", contentType, body);
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "socket 创建失败\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "bind 失败\n";
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        cerr << "listen 失败\n";
        close(server_fd);
        return 1;
    }

    cout << "静态文件服务器已启动：http://127.0.0.1:8080\n";
    cout << "网站根目录: " << WEB_ROOT << "/\n";

    while(true)
    {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            cerr << "accept 失败\n";
            continue;
        }

        cout << "收到新连接\n";
        char buffer[8192] = {0};
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

        string response;
        if (n <= 0) {
            response = buildHttpResponse(400, "Bad Request",
                "text/plain; charset=utf-8", "无法读取请求");
        } else {
            string rawRequest(buffer, n);
            HttpRequest req;
            if (!parseRequestLine(rawRequest, req)) {
                response = buildHttpResponse(400, "Bad Request",
                    "text/plain; charset=utf-8", "请求行格式错误");
            } else {
                cout << "解析结果: " << req.method << " " << req.path << "\n";
                cout << "读取文件: " << pathToFile(req.path) << "\n";
                response = handleRequest(req);
            }
        }

        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "已响应并关闭连接\n";
    }
    close(server_fd);
    return 0;
}