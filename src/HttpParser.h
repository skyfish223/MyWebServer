#pragma once

#include "HttpTypes.h"
#include <string>

class HttpConnection;

bool get_line(std::string& buf, std::string& line);
bool parse_request_line(const std::string& line, HttpRequest& req);
void parse_header_line(const std::string& line, HttpRequest& req);
ReadResult process_read(HttpConnection& conn);
bool append_read(HttpConnection& conn);