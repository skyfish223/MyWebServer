#include "HttpParser.h"
#include "HttpConnection.h"
#include "ServerConfig.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <cerrno>

using namespace std;

static string trim(const string& s)
{
    size_t start = s.find_first_not_of(" \t");
    if(start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static string toLower(string s)
{
    for(char& c : s)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool get_line(string& buf, string& line)
{
    size_t pos = buf.find("\r\n");
    if(pos == string::npos) return false;
    line = buf.substr(0, pos);
    buf.erase(0, pos + 2);
    return true;
}

bool parse_request_line(const string& line, HttpRequest& req)
{
    istringstream iss(line);
    return static_cast<bool>(iss >> req.method >> req.path >> req.version);
}

void parse_header_line(const string& line, HttpRequest& req)
{
    size_t colon = line.find(":");
    if(colon == string::npos) return;
    string key = toLower(trim(line.substr(0, colon)));
    string val = trim(line.substr(colon + 1));
    req.headers[key] = val;
    if(key == "content-length")
    {
        req.content_length = static_cast<size_t>(stoul(val));
    }
}

ReadResult process_read(HttpConnection& conn)
{
    string line;
    while(true)
    {
        if(conn.state_ == ParseState::Content)
        {
            if(conn.request_.content_length > g_cfg.max_body_size)
                return ReadResult::Error;
            if(conn.read_buf_.size() >= conn.request_.content_length)
            {
                conn.request_.body = conn.read_buf_.substr(0, conn.request_.content_length);
                conn.read_buf_.erase(0, conn.request_.content_length);
                return ReadResult::Complete;
            }
            return ReadResult::Incomplete;
        }
            if(!get_line(conn.read_buf_, line))
        {
            return ReadResult::Incomplete;
        }

        switch(conn.state_)
        {
            case ParseState::RequestLine:
                if(!parse_request_line(line, conn.request_))
                    return ReadResult::Error;
                conn.state_ = ParseState::Header;
                break;

            case ParseState::Header:
                if(line.empty())
                {
                    if(conn.request_.method == "POST" && conn.request_.content_length > 0)
                    {
                        conn.state_ = ParseState::Content;
                    }
                    else
                    {
                        return ReadResult::Complete;
                    }
                }
                else
                {
                    parse_header_line(line, conn.request_);
                }
                break;

            case ParseState::Content:
                break;
        }
    }
}

bool append_read(HttpConnection& conn)
{
    char buffer[8192];
    ssize_t n = read(conn.fd_, buffer, sizeof(buffer));
    if(n < 0)
    {
        if(errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
    if(n == 0) return false;
    conn.read_buf_.append(buffer, static_cast<size_t>(n));
    return true;
}