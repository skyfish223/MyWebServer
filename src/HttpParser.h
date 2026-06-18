#pragma once

#include "HttpTypes.h"
#include <string>

bool parseRequestLine(const std::string& raw, HttpRequest& req);