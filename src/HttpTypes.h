#pragma once

#include <string>
#include <map>

using namespace std;

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

enum class WriteResult
{
    Incomplete,
    Complete,
    Error
};

enum class ConnPhase
{
    Reading,
    WaitingResponse,
    Writing
};

struct HttpRequest
{
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::size_t content_length = 0;
    std::string body;
};

struct ReadyResponse
{
    int fd;
    std::string data;
};