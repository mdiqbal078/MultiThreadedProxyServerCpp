#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>
#include <csignal>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "event_loop.hpp"
#include "buffer.hpp"
#include "dns_resolver.hpp"
#include "lru_cache.hpp"
#include "metrics.hpp"
#include "acl.hpp"
#include "rate_limiter.hpp"
#include "proxy_parse.hpp"

using namespace std::chrono_literals;

// Global Singletons
Metrics g_metrics;
RateLimiter g_rate_limiter(100000.0, 100000.0); // High limit for benchmarking
ACL g_acl;
LRUCache g_cache;

enum class State {
    READING_REQUEST,
    RESOLVING_DNS,
    CONNECTING_UPSTREAM,
    FORWARDING,
    CLOSING
};

struct Connection {
    int id;
    int client_fd = -1;
    int upstream_fd = -1;
    std::string client_ip;
    State state = State::READING_REQUEST;
    
    Buffer client_in;
    Buffer client_out;
    Buffer upstream_in;
    Buffer upstream_out;

    std::chrono::steady_clock::time_point last_active;

    ParsedRequest request;
    bool is_connect_method = false;
    std::string cache_key;
    bool cacheable = false;
    std::vector<char> cache_buffer;
};

// Set socket to non-blocking
bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

// A single Reactor thread managing its own event loop and connections
class Reactor {
    EventLoop loop_;
    DNSResolver dns_;
    int listen_fd_;
    
    std::unordered_map<int, std::unique_ptr<Connection>> conns_; // by id
    std::unordered_map<int, Connection*> fd_to_conn_;            // by fd
    int next_conn_id_ = 1;
    
    std::chrono::steady_clock::time_point last_sweep_;

    void close_connection(Connection* conn) {
        if (!conn) return;
        if (conn->client_fd >= 0) {
            loop_.remove(conn->client_fd);
            close(conn->client_fd);
            fd_to_conn_.erase(conn->client_fd);
            g_metrics.active_connections--;
        }
        if (conn->upstream_fd >= 0) {
            loop_.remove(conn->upstream_fd);
            close(conn->upstream_fd);
            fd_to_conn_.erase(conn->upstream_fd);
        }
        conns_.erase(conn->id);
    }

    void handle_accept() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) return;

        set_nonblocking(client_fd);
        
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        auto conn = std::make_unique<Connection>();
        conn->id = next_conn_id_++;
        conn->client_fd = client_fd;
        conn->last_active = std::chrono::steady_clock::now();

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        conn->client_ip = client_ip;

        fd_to_conn_[client_fd] = conn.get();
        conns_[conn->id] = std::move(conn);

        loop_.add(client_fd, EV_READABLE);
        g_metrics.active_connections++;
        g_metrics.total_requests++;
    }

    void handle_dns_result(const DNSResult& result) {
        auto it = conns_.find(result.connection_id);
        if (it == conns_.end()) {
            if (result.addr_info) freeaddrinfo(result.addr_info);
            return;
        }
        Connection* conn = it->second.get();
        conn->last_active = std::chrono::steady_clock::now();

        if (result.error != "" || !result.addr_info) {
            std::string err = "HTTP/1.1 502 Bad Gateway\r\n\r\nDNS Resolution Failed";
            conn->client_out.append(err.data(), err.size());
            conn->state = State::CLOSING;
            loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            if (result.addr_info) freeaddrinfo(result.addr_info);
            return;
        }

        int upstream_fd = socket(result.addr_info->ai_family, result.addr_info->ai_socktype, result.addr_info->ai_protocol);
        if (upstream_fd < 0) {
            freeaddrinfo(result.addr_info);
            close_connection(conn);
            return;
        }

        set_nonblocking(upstream_fd);
        conn->upstream_fd = upstream_fd;
        fd_to_conn_[upstream_fd] = conn;

        int ret = connect(upstream_fd, result.addr_info->ai_addr, result.addr_info->ai_addrlen);
        freeaddrinfo(result.addr_info);

        if (ret == 0) {
            // Connected immediately
            on_upstream_connected(conn);
        } else if (errno == EINPROGRESS) {
            conn->state = State::CONNECTING_UPSTREAM;
            loop_.add(upstream_fd, EV_WRITABLE);
        } else {
            close_connection(conn);
        }
    }

    void on_upstream_connected(Connection* conn) {
        conn->state = State::FORWARDING;
        if (conn->is_connect_method) {
            std::string ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
            conn->client_out.append(ok.data(), ok.size());
            loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            g_metrics.connect_tunnels++;
        } else {
            std::string req = conn->request.unparse();
            conn->upstream_out.append(req.data(), req.size());
            if (conn->client_in.size() > 0) {
                conn->upstream_out.append(conn->client_in.data(), conn->client_in.size());
                conn->client_in.clear();
            }
        }
        
        loop_.modify(conn->upstream_fd, EV_READABLE | EV_WRITABLE);
    }

    void handle_readable(Connection* conn, int fd) {
        char buf[8192];
        ssize_t n = read(fd, buf, sizeof(buf));
        
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close_connection(conn);
            }
            return;
        }

        if (n == 0) {
            // EOF
            if (fd == conn->upstream_fd && conn->cacheable) {
                g_cache.put(conn->cache_key, conn->cache_buffer);
            }
            conn->state = State::CLOSING;
            if (fd == conn->client_fd) {
                loop_.modify(conn->client_fd, EV_WRITABLE); // Drain out then close
            } else {
                loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            }
            return;
        }

        conn->last_active = std::chrono::steady_clock::now();

        if (fd == conn->client_fd) {
            if (conn->state == State::READING_REQUEST) {
                conn->client_in.append(buf, n);
                std::string_view view = conn->client_in.view();
                if (view.find("\r\n\r\n") != std::string_view::npos) {
                    process_request(conn);
                }
            } else if (conn->state == State::FORWARDING) {
                conn->upstream_out.append(buf, n);
                loop_.modify(conn->upstream_fd, EV_READABLE | EV_WRITABLE);
            }
        } else if (fd == conn->upstream_fd) {
            if (conn->state == State::FORWARDING) {
                conn->client_out.append(buf, n);
                if (conn->cacheable && conn->cache_buffer.size() + n < 10*1024*1024) {
                    conn->cache_buffer.insert(conn->cache_buffer.end(), buf, buf + n);
                } else {
                    conn->cacheable = false; // Too large to cache
                }
                loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            }
        }
    }

    void process_request(Connection* conn) {
        if (conn->request.parse(conn->client_in.data(), conn->client_in.size()) < 0) {
            std::string err = "HTTP/1.1 400 Bad Request\r\n\r\n";
            conn->client_out.append(err.data(), err.size());
            conn->state = State::CLOSING;
            loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            return;
        }
        conn->client_in.clear();

        // ACL Check
        if (g_acl.is_blocked(conn->request.host)) {
            std::string err = "HTTP/1.1 403 Forbidden\r\n\r\nBlocked by ACL";
            conn->client_out.append(err.data(), err.size());
            conn->state = State::CLOSING;
            loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            g_metrics.blocked_by_acl++;
            return;
        }

        // Rate Limit per IP
        if (!g_rate_limiter.allow(conn->client_ip)) { 
            std::string err = "HTTP/1.1 429 Too Many Requests\r\n\r\nRate Limited";
            conn->client_out.append(err.data(), err.size());
            conn->state = State::CLOSING;
            loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
            g_metrics.rate_limited++;
            return;
        }

        if (conn->request.method == "CONNECT") {
            conn->is_connect_method = true;
            conn->state = State::RESOLVING_DNS;
            dns_.submit(conn->id, conn->request.host, conn->request.port);
            return;
        }

        // Cache Check (only GET)
        if (conn->request.method == "GET") {
            conn->cache_key = conn->request.host + ":" + conn->request.port + conn->request.path;
            auto cached = g_cache.get(conn->cache_key);
            if (cached) {
                g_metrics.cache_hits++;
                conn->client_out.append(cached->data(), cached->size());
                conn->state = State::CLOSING;
                loop_.modify(conn->client_fd, EV_READABLE | EV_WRITABLE);
                return;
            }
            g_metrics.cache_misses++;
            conn->cacheable = true;
        }

        // Force Connection: close to ensure upstream doesn't keep-alive (fixes ab timeout)
        conn->request.set_header("Connection", "close");

        conn->state = State::RESOLVING_DNS;
        std::string port = conn->request.port.empty() ? "80" : conn->request.port;
        dns_.submit(conn->id, conn->request.host, port);
    }

    void handle_writable(Connection* conn, int fd) {
        conn->last_active = std::chrono::steady_clock::now();

        if (fd == conn->upstream_fd && conn->state == State::CONNECTING_UPSTREAM) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                close_connection(conn);
                return;
            }
            on_upstream_connected(conn);
            return;
        }

        Buffer& buf = (fd == conn->client_fd) ? conn->client_out : conn->upstream_out;
        if (!buf.empty()) {
            ssize_t n = write(fd, buf.data(), buf.size());
            if (n > 0) {
                buf.consume(n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close_connection(conn);
                return;
            }
        }

        if (buf.empty()) {
            if (conn->state == State::CLOSING) {
                close_connection(conn);
            } else {
                loop_.modify(fd, EV_READABLE); // Stop polling for write if buffer empty
            }
        }
    }

    void sweep_idle_connections() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_sweep_ < 5s) return;
        last_sweep_ = now;

        std::vector<Connection*> to_close;
        for (auto& [id, conn] : conns_) {
            if (now - conn->last_active > 30s) {
                to_close.push_back(conn.get());
            }
        }
        for (auto* c : to_close) {
            close_connection(c);
        }
    }

public:
    Reactor(int listen_fd) 
        : dns_(2), listen_fd_(listen_fd), last_sweep_(std::chrono::steady_clock::now()) {
        loop_.add(listen_fd, EV_READABLE);
        loop_.add(dns_.notify_fd(), EV_READABLE);
    }

    void run() {
        std::vector<PollEvent> events;
        while (true) {
            int n = loop_.wait(events, 1000); // 1s timeout
            if (n < 0) continue;

            for (const auto& ev : events) {
                if (ev.fd == listen_fd_ && (ev.events & EV_READABLE)) {
                    handle_accept();
                } else if (ev.fd == dns_.notify_fd() && (ev.events & EV_READABLE)) {
                    auto results = dns_.drain();
                    for (const auto& r : results) {
                        handle_dns_result(r);
                    }
                } else {
                    auto it = fd_to_conn_.find(ev.fd);
                    if (it != fd_to_conn_.end()) {
                        Connection* conn = it->second;
                        if (ev.events & EV_READABLE) {
                            handle_readable(conn, ev.fd);
                        }
                        // Check again because readable might have closed it
                        if (fd_to_conn_.count(ev.fd) && (ev.events & EV_WRITABLE)) {
                            handle_writable(conn, ev.fd);
                        }
                    }
                }
            }
            sweep_idle_connections();
        }
    }
};

// Simple metrics HTTP endpoint on a separate port
void run_metrics_server() {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8081);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    bind(sfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sfd, 10);
    
    while(true) {
        int client = accept(sfd, nullptr, nullptr);
        if (client < 0) continue;
        
        std::string json = g_metrics.to_json(g_cache.eviction_stats().total_count(),
                                             g_cache.eviction_stats().average_us());
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + json;
        write(client, res.data(), res.size());
        close(client);
    }
}

int main(int argc, char** argv) {
    // Ignore SIGPIPE to prevent crashes on partial writes to closed sockets
    signal(SIGPIPE, SIG_IGN);

    int port = 8080;
    if (argc > 1) port = std::stoi(argv[1]);

    std::cout << "Starting Robust Multi-Reactor Proxy on port " << port << std::endl;
    std::cout << "Metrics available at http://localhost:8081/" << std::endl;

    g_acl.block("blocked.com");

    std::thread metrics_thread(run_metrics_server);
    metrics_thread.detach();

    int num_reactors = std::thread::hardware_concurrency();
    if (num_reactors == 0) num_reactors = 4;
    
    std::vector<std::thread> threads;
    for (int i = 0; i < num_reactors; i++) {
        threads.emplace_back([port]() {
            int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                perror("bind");
                exit(1);
            }
            listen(listen_fd, 4096);
            
            Reactor reactor(listen_fd);
            reactor.run();
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    return 0;
}
