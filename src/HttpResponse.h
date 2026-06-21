#pragma once

#include <string>

std::string buildHttpResponse(int statusCode,
                            const std::string& statusText,
                            const std::string& contentType,
                            const std::string& body);

std::string pathToFile(const std::string& urlPath);

bool isPathSafe(const std::string& urlPath);

std::string getContentType(const std::string& filePath);

bool readFile(const std::string& filePath, std::string& out);

std::string buildRedirectResponse(const std::string& location);