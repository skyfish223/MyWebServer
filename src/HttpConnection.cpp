#include "HttpConnection.h"
#include "ThreadPool.h"
#include "EpollHelper.h"
#include "HttpParser.h"
#include "HttpHandler.h"
#include "HttpResponse.h"
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std;

static string processRawRequest(const string& rawRequest)
{
    HttpRequest req;
    if(!parseRequestLine(rawRequest, req))
    {
        return buildHttpResponse(400, "Bad Request",
            "text/plain; charset=utf-8", "请求行格式错误");
    }
    cout << "[worker " << this_thread::get_id() << "]"
        << req.method << " " << req.path << endl;
    return handleRequest(req);
}

void dispatchClient(int epfd, int client_fd, ThreadPool& pool)
{
    char buffer[8192] = {0};
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    
    removeFd(epfd, client_fd);

    string rawRequest;
    if(n > 0)
    {
        rawRequest.assign(buffer, static_cast<size_t>(n));
    }

    pool.submit([client_fd, rawRequest, n] () {
        string response;
        if(n <= 0)
        {
            response = buildHttpResponse(400, "Bad Request",
                "text/plain; charset=utf=8", "无法读取请求");
        }
        else
        {
            response = processRawRequest(rawRequest);
        }
        write(client_fd, response.c_str(), response.size());
        close(client_fd);
        cout << "[worker] 完成 fd=" << client_fd << "\n";
    });
}