/*
 * proxy_parse.cpp -- a HTTP Request Parsing Library (Modern C++ version).
 */

#include "proxy_parse.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <string_view>

constexpr size_t MIN_REQ_LEN = 4;
constexpr size_t MAX_REQ_LEN = 65535;

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](unsigned char c1, unsigned char c2) {
                          return std::tolower(c1) == std::tolower(c2);
                      });
}

} // namespace

void ParsedRequest::set_header(const std::string& key, const std::string& value) {
    remove_header(key);
    headers.push_back({key, value});
}

std::optional<std::string> ParsedRequest::get_header(const std::string& key) const {
    for (const auto& header : headers) {
        if (iequals(header.key, key)) {
            return header.value;
        }
    }
    return std::nullopt;
}

bool ParsedRequest::remove_header(const std::string& key) {
    auto it = std::find_if(headers.begin(), headers.end(),
        [&key](const ParsedHeader& h) { return iequals(h.key, key); });
    
    if (it != headers.end()) {
        headers.erase(it);
        return true;
    }
    return false;
}

std::string ParsedRequest::requestLineToString() const {
    std::ostringstream oss;
    
    if (!method.empty()) oss << method << " ";
    
    if (!protocol.empty()) oss << protocol << "://";
    if (!host.empty()) oss << host;
    if (!port.empty()) oss << ":" << port;
    if (!path.empty()) oss << path;
    
    oss << " ";
    if (!version.empty()) oss << version;
    
    oss << "\r\n";
    return oss.str();
}

std::string ParsedRequest::unparse_headers() const {
    std::ostringstream oss;
    for (const auto& header : headers) {
        oss << header.key << ": " << header.value << "\r\n";
    }
    oss << "\r\n";
    return oss.str();
}

std::string ParsedRequest::unparse() const {
    return requestLineToString() + unparse_headers();
}

int ParsedRequest::parse(const char* buf, size_t buflen) {
    if (buflen < MIN_REQ_LEN || buflen > MAX_REQ_LEN) {
        debug("invalid buflen ", buflen, "\n");
        return -1;
    }

    std::string_view req(buf, buflen);

    size_t end_of_headers = req.find("\r\n\r\n");
    if (end_of_headers == std::string_view::npos) {
        debug("invalid request, no end of header\n");
        return -1;
    }

    size_t end_of_req_line = req.find("\r\n");
    if (end_of_req_line == std::string_view::npos) {
        debug("could not find end of request line\n");
        return -1;
    }

    std::string_view req_line = req.substr(0, end_of_req_line);
    
    // Parse method
    size_t space1 = req_line.find(' ');
    if (space1 == std::string_view::npos) {
        debug("invalid request line, no URI\n");
        return -1;
    }
    method = std::string(req_line.substr(0, space1));

    // Parse version
    size_t space2 = req_line.rfind(' ');
    if (space2 == std::string_view::npos || space2 == space1) {
        debug("invalid request line, no version\n");
        return -1;
    }
    version = std::string(req_line.substr(space2 + 1));

    // Parse URI
    std::string_view uri = req_line.substr(space1 + 1, space2 - space1 - 1);

    // Protocol
    size_t proto_end = uri.find("://");
    if (proto_end != std::string_view::npos) {
        protocol = std::string(uri.substr(0, proto_end));
        uri.remove_prefix(proto_end + 3);
    }

    // Path
    size_t path_start = uri.find('/');
    if (path_start != std::string_view::npos) {
        path = std::string(uri.substr(path_start));
        uri.remove_suffix(uri.length() - path_start);
    } else {
        path = "/";
    }

    // Host and Port
    size_t port_start = uri.find(':');
    if (port_start != std::string_view::npos) {
        port = std::string(uri.substr(port_start + 1));
        host = std::string(uri.substr(0, port_start));
    } else {
        host = std::string(uri);
    }

    // Parse headers
    size_t header_start = end_of_req_line + 2;
    while (header_start < end_of_headers) {
        size_t line_end = req.find("\r\n", header_start);
        if (line_end == std::string_view::npos || line_end == header_start) {
            break; // Empty line
        }

        std::string_view header_line = req.substr(header_start, line_end - header_start);
        size_t colon = header_line.find(':');
        
        if (colon != std::string_view::npos) {
            std::string_view hkey = header_line.substr(0, colon);
            std::string_view hval = header_line.substr(colon + 1);
            
            // Trim leading spaces from value
            while (!hval.empty() && hval.front() == ' ') {
                hval.remove_prefix(1);
            }
            
            set_header(std::string(hkey), std::string(hval));
        }

        header_start = line_end + 2;
    }

    return 0;
}
