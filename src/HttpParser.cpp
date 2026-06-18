#include "HttpParser.h"
#include <sstream>

using namespace std;

bool parseRequestLine(const string& raw, HttpRequest& req)
{
    size_t lineEnd = raw.find("\r\n");
    if(lineEnd == string::npos)
    {
        lineEnd = raw.find("\n");
    }
    if(lineEnd == string::npos)
    {
        return false;
    }

    string line = raw.substr(0, lineEnd);

    istringstream iss(line);
    if(!(iss >> req.method >> req.path >> req.version))
    {
        return false;
    }
    return true;
}