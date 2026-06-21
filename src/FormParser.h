#pragma once

#include <string>
#include <map>

std::string url_decode(const std::string& s);
std::map<std::string, std::string> parse_form_urlencoded(const std::string& body);
std::string escape_sql(const std::string& s);