#pragma once

#include "HttpTypes.h"
#include <string>

std::string handleRequest(const HttpRequest& req);
std::string do_request(const HttpRequest& req);

std::string handle_register(const HttpRequest& req);
std::string handle_login(const HttpRequest& req);