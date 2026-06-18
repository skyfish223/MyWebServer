#pragma once

#include <string>

using namespace std;

struct HttpRequest
{
    std::string method;
    std::string path;
    std::string version;
};