/*
 * proxy_server_with_cache.cpp -- Multi-Threaded Proxy Server (Modern C++20).
 * Features:
 *   - RAII Socket wrapper to prevent FD leaks
 *   - O(1) LRU Cache using std::list and std::unordered_map
 *   - Modern threading (std::thread, std::mutex, std::counting_semaphore)
 */

#include "proxy_parse.hpp"
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <list>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <thread>
#include <mutex>
#include <semaphore>
#include <chrono>
#include <cstring>
#include <memory>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr int MAX_BYTES = 4096;
constexpr int MAX_CLIENTS = 400;
constexpr int MAX_SIZE = 200 * (1 << 20);            // 200 MB global limit
constexpr int MAX_ELEMENT_SIZE = 10 * (1 << 20);     // 10 MB per element limit

// ============================================================================
// RAII Socket Wrapper
// ============================================================================
class Socket {
    int fd;
public:
    explicit Socket(int fd = -1) : fd(fd) {}
    
    ~Socket() {
        if (fd != -1) {
            close(fd);
        }
    }
    
    // Disable copy
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    
    // Enable move
    Socket(Socket&& other) noexcept : fd(other.fd) {
        other.fd = -1;
    }
    
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (fd != -1) close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    
    int get() const { return fd; }
    bool is_valid() const { return fd != -1; }
};

// ============================================================================
// O(1) LRU Cache
// ============================================================================
struct CacheElement {
    std::string url;
    std::vector<char> data;
};

class LRUCache {
private:
    std::list<CacheElement> lru_list;
    std::unordered_map<std::string, decltype(lru_list)::iterator> cache_map;
    std::mutex cache_mutex;
    size_t current_cache_size = 0;

    void evict_lru() {
        if (lru_list.empty()) return;
        
        auto last = lru_list.end();
        --last; // get the last element (least recently used)
        
        current_cache_size -= (last->data.size() + last->url.size());
        cache_map.erase(last->url);
        lru_list.pop_back();
    }

public:
    std::optional<std::vector<char>> get(const std::string& url) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        auto it = cache_map.find(url);
        if (it == cache_map.end()) {
            return std::nullopt; // Cache miss
        }
        
        // Cache hit: Move element to the front of the list
        lru_list.splice(lru_list.begin(), lru_list, it->second);
        
        return it->second->data;
    }

    void put(const std::string& url, const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        size_t elem_size = data.size() + url.size();
        if (elem_size > MAX_ELEMENT_SIZE) {
            return; // Too big to cache
        }
        
        auto it = cache_map.find(url);
        if (it != cache_map.end()) {
            // Update existing
            current_cache_size -= it->second->data.size();
            it->second->data = data;
            current_cache_size += data.size();
            lru_list.splice(lru_list.begin(), lru_list, it->second);
        } else {
            // Insert new
            while (current_cache_size + elem_size > MAX_SIZE && !lru_list.empty()) {
                evict_lru();
            }
            
            lru_list.push_front({url, data});
            cache_map[url] = lru_list.begin();
            current_cache_size += elem_size;
            std::cout << "Cached: " << url << " (Size: " << data.size() << " bytes)\n";
        }
    }
};

// ============================================================================
// Global State
// ============================================================================
LRUCache proxy_cache;
std::counting_semaphore<MAX_CLIENTS> connection_semaphore(MAX_CLIENTS);

// ============================================================================
// Helper Functions
// ============================================================================
void sendErrorMessage(int socket, int status_code) {
    std::string response;
    
    time_t now = time(nullptr);
    char currentTime[50];
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", gmtime(&now));

    switch (status_code) {
        case 400:
            response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: close\r\nContent-Type: text/html\r\nDate: " + std::string(currentTime) + "\r\nServer: CppProxy\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>";
            break;
        case 403:
            response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nConnection: close\r\nContent-Type: text/html\r\nDate: " + std::string(currentTime) + "\r\nServer: CppProxy\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>";
            break;
        case 404:
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nConnection: close\r\nContent-Type: text/html\r\nDate: " + std::string(currentTime) + "\r\nServer: CppProxy\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>";
            break;
        case 500:
            response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: close\r\nContent-Type: text/html\r\nDate: " + std::string(currentTime) + "\r\nServer: CppProxy\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>";
            break;
        case 501:
            response = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: close\r\nContent-Type: text/html\r\nDate: " + std::string(currentTime) + "\r\nServer: CppProxy\r\n\r\n<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>";
            break;
        default:
            return;
    }
    send(socket, response.c_str(), response.length(), 0);
}

Socket connectRemoteServer(const std::string& host_addr, int port_num) {
    // Use getaddrinfo (thread-safe, supports IPv4 & IPv6) instead of deprecated gethostbyname
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP

    std::string port_str = std::to_string(port_num);
    struct addrinfo* result = nullptr;

    int status = getaddrinfo(host_addr.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(status) << "\n";
        return Socket(-1);
    }

    // Try each address until we successfully connect
    for (struct addrinfo* p = result; p != nullptr; p = p->ai_next) {
        int remote_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (remote_fd < 0) continue;

        Socket remoteSocket(remote_fd);
        if (connect(remoteSocket.get(), p->ai_addr, p->ai_addrlen) == 0) {
            freeaddrinfo(result);
            return remoteSocket; // Success — transfer ownership to caller
        }
        // Socket destructor auto-closes on failure, try next address
    }

    freeaddrinfo(result);
    std::cerr << "Failed to connect to " << host_addr << ":" << port_num << "\n";
    return Socket(-1);
}

bool checkHTTPversion(const std::string& version) {
    return version == "HTTP/1.0" || version == "HTTP/1.1";
}

// ============================================================================
// Request Handler
// ============================================================================
// Build a canonical URL from parsed request fields for use as the cache key.
// This strips browser-specific headers (User-Agent, etc.) so Chrome and Firefox
// hitting the same URL will produce the same cache key.
std::string build_cache_key(const ParsedRequest& req) {
    std::string key;
    if (!req.protocol.empty()) {
        key += req.protocol + "://";
    } else {
        key += "http://";
    }
    key += req.host;
    if (!req.port.empty() && req.port != "80") {
        key += ":" + req.port;
    }
    key += req.path;
    return key;
}

void handle_request(Socket client_socket) {
    // Release semaphore when this function exits (thread completes)
    struct SemaphoreReleaser {
        ~SemaphoreReleaser() { connection_semaphore.release(); }
    } releaser;

    std::vector<char> buffer(MAX_BYTES, 0);
    int bytes_received = recv(client_socket.get(), buffer.data(), MAX_BYTES, 0);
    
    if (bytes_received <= 0) return;

    // Read full headers (wait for \r\n\r\n)
    int total_bytes = bytes_received;
    while (total_bytes < MAX_BYTES) {
        std::string_view current_req(buffer.data(), total_bytes);
        if (current_req.find("\r\n\r\n") != std::string_view::npos) {
            break;
        }
        int more = recv(client_socket.get(), buffer.data() + total_bytes, MAX_BYTES - total_bytes, 0);
        if (more <= 0) break;
        total_bytes += more;
    }

    // Parse the request FIRST so we can build a proper cache key
    ParsedRequest req;
    if (req.parse(buffer.data(), total_bytes) < 0) {
        sendErrorMessage(client_socket.get(), 400);
        return;
    }

    if (req.method != "GET") {
        std::cout << "Method " << req.method << " not supported.\n";
        sendErrorMessage(client_socket.get(), 501);
        return;
    }

    if (!checkHTTPversion(req.version)) {
        sendErrorMessage(client_socket.get(), 505);
        return;
    }

    // Build canonical cache key from the parsed URL (not the raw request)
    std::string cache_key = build_cache_key(req);

    // 1. Check Cache using the canonical URL key
    auto cached_data = proxy_cache.get(cache_key);
    if (cached_data) {
        std::cout << "Cache HIT for " << cache_key << "\n";
        const auto& data = cached_data.value();
        
        // Stream data back to client
        size_t pos = 0;
        while (pos < data.size()) {
            size_t chunk = std::min<size_t>(MAX_BYTES, data.size() - pos);
            if (send(client_socket.get(), data.data() + pos, chunk, 0) < 0) {
                break;
            }
            pos += chunk;
        }
        return;
    }

    // 2. Cache Miss - Forward request to remote server
    std::cout << "Cache MISS for " << cache_key << "\n";

    // Modify request for upstream
    req.set_header("Connection", "close");
    if (!req.get_header("Host")) {
        req.set_header("Host", req.host);
    }
    
    std::string upstream_req = req.unparse();

    int remote_port = req.port.empty() ? 80 : std::stoi(req.port);
    Socket remote_socket = connectRemoteServer(req.host, remote_port);

    if (!remote_socket.is_valid()) {
        sendErrorMessage(client_socket.get(), 500);
        return;
    }

    // Send to remote server
    if (send(remote_socket.get(), upstream_req.c_str(), upstream_req.length(), 0) < 0) {
        sendErrorMessage(client_socket.get(), 500);
        return;
    }

    // Receive from remote, stream to client, and cache
    std::vector<char> response_data;
    char recv_buf[MAX_BYTES];
    
    while (true) {
        int bytes = recv(remote_socket.get(), recv_buf, MAX_BYTES, 0);
        if (bytes <= 0) break;
        
        send(client_socket.get(), recv_buf, bytes, 0);
        response_data.insert(response_data.end(), recv_buf, recv_buf + bytes);
    }

    // Cache the response using the canonical URL key
    if (!response_data.empty()) {
        proxy_cache.put(cache_key, response_data);
    }
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    int port_number = 8080;
    if (argc == 2) {
        port_number = std::stoi(argv[1]);
    } else if (argc > 2) {
        std::cout << "Usage: ./proxy <port>\n";
        return 1;
    }

    std::cout << "Starting Modern C++20 Proxy Server on port " << port_number << "\n";

    Socket listen_socket(socket(AF_INET, SOCK_STREAM, 0));
    if (!listen_socket.is_valid()) {
        std::cerr << "Failed to create socket.\n";
        return 1;
    }

    int reuse = 1;
    setsockopt(listen_socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_socket.get(), reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        std::cerr << "Port binding failed.\n";
        return 1;
    }

    if (listen(listen_socket.get(), MAX_CLIENTS) < 0) {
        std::cerr << "Listen failed.\n";
        return 1;
    }

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_socket.get(), reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            std::cerr << "Accept failed.\n";
            continue;
        }

        // Acquire semaphore token (blocks if 400 clients are active)
        connection_semaphore.acquire();

        // Wrap the socket descriptor in our RAII class
        Socket client_socket(client_fd);
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        std::cout << "Accepted connection from " << ip_str << ":" << ntohs(client_addr.sin_port) << "\n";

        // Spawn detached std::thread and move ownership of the socket into it
        std::thread([client = std::move(client_socket)]() mutable {
            handle_request(std::move(client));
        }).detach();
    }

    return 0;
}
