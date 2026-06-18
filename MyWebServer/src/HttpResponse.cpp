#include "HttpResponse.h"
#include "ServerConfig.h"
#include <string>
#include <fstream>
#include <iterator>

using namespace std;

string buildHttpResponse(int statusCode,
                        const std::string& statusText,
                        const std::string& contentType,
                        const std::string& body)
{
    return "HTTP/1.1" + to_string(statusCode) + " " + statusText + "\r\n"
            "Content-Type: " + contentType + "\r\n"
            "Content_Length: " + to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + 
            body;
}

string pathToFile(const string& urlPath)
{
    if(urlPath == "/" || urlPath == "/index.html")
    {
        return g_cfg.web_root + "/index.html";
    }
    return g_cfg.web_root + urlPath;
}

bool isPathSafe(const string& urlPath)
{
    if(urlPath.find("..") != string::npos)
    {
        return false;
    }
    return true;
}

string getContentType(const string& filePath)
{
    if(filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".html")
    {
        return "text/html; chatset=utf-8";
    }
    if(filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".css")
    {
        return "text/css; charset=utf-8";
    }
    if(filePath.size() >= 3 && filePath.substr(filePath.size() - 3) == ".js")
    {
        return "application/javascript; charset=utf-8";
    }
    if(filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".png")
    {
        return "image/png";
    }
    if(filePath.size() >= 4 && filePath.substr(filePath.size() - 4) == ".jpg")
    {
        return "image/jpeg";
    }
    if(filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".jpeg")
    {
        return "image/jpeg";
    }
    return "application/octet-stream";
}

bool readFile(const string& filePath, string& out)
{
    ifstream ifs(filePath, ios::binary);
    if(!ifs)
    {
        return false;
    }
    out.assign(istreambuf_iterator<char>(ifs), istreambuf_iterator<char>());
    return true;
}