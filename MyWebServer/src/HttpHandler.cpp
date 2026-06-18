#include "HttpHandler.h"
#include "HttpResponse.h"

using namespace std;

string handleRequest(const HttpRequest& req)
{
    if(req.method != "GET")
    {
        return buildHttpResponse(
            405, "Method Not Allowed", 
            "text/plain; charset=utf-8",
            "暂只支持 GET 请求，收到的是： " + req.method
        );
    }

    if(!isPathSafe(req.path))
    {
        return buildHttpResponse(
            403, "Forbidden",
            "text/plain; charset=utf-8",
            "403 - 非法路径"
        );
    }

    string filePath = pathToFile(req.path);
    string body;
    if(!readFile(filePath, body))
    {
        return buildHttpResponse(
            404, "Not Found", 
            "text/plain; charset=utf-8",
            "404 - 文件不存在：" + req.path
        );
    }

    string contentType = getContentType(filePath);
    return buildHttpResponse(200, "OK", contentType, body);
}