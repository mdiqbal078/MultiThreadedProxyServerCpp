/*
 * proxy_parse.hpp -- a HTTP Request Parsing Library (Modern C++ version).
 */

#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <utility>

constexpr bool DEBUG = true;

struct ParsedHeader {
    std::string key;
    std::string value;
};

class ParsedRequest {
public:
    std::string method;
    std::string protocol;
    std::string host;
    std::string port;
    std::string path;
    std::string version;
    std::vector<ParsedHeader> headers;

    ParsedRequest() = default;
    ~ParsedRequest() = default;

    // Parse the request buffer. Returns 0 on success, -1 on failure.
    int parse(const char* buf, size_t buflen);

    // Retrieve the entire buffer from a parsed request object.
    // Returns the serialized HTTP request string.
    std::string unparse() const;

    // Retrieve the entire buffer from a parsed request object used for the headers.
    std::string unparse_headers() const;

    // Header operations
    void set_header(const std::string& key, const std::string& value);
    std::optional<std::string> get_header(const std::string& key) const;
    bool remove_header(const std::string& key);

private:
    std::string requestLineToString() const;
};

// Debug utility
template<typename... Args>
void debug(Args&&... args) {
    if (DEBUG) {
        ((std::cerr << std::forward<Args>(args)), ...);
    }
}
