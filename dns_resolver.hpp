/*
 * dns_resolver.hpp -- Async DNS resolution thread pool.
 *
 * Blocking getaddrinfo() calls are offloaded to a small thread pool.
 * When resolution completes, a byte is written to a pipe to wake the
 * reactor's event loop, which then drains the result queue.
 */

#pragma once

#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

struct DNSRequest {
    int connection_id;
    std::string host;
    std::string port;
};

struct DNSResult {
    int connection_id;
    struct addrinfo* addr_info = nullptr;  // Caller must freeaddrinfo()
    std::string error;
};

class DNSResolver {
    std::vector<std::thread> workers_;

    std::queue<DNSRequest> request_queue_;
    std::mutex request_mutex_;
    std::condition_variable request_cv_;

    std::queue<DNSResult> result_queue_;
    std::mutex result_mutex_;

    int notify_pipe_[2] = {-1, -1};
    bool stop_ = false;

    void worker_loop() {
        while (true) {
            DNSRequest req;
            {
                std::unique_lock<std::mutex> lock(request_mutex_);
                request_cv_.wait(lock, [this] {
                    return stop_ || !request_queue_.empty();
                });
                if (stop_ && request_queue_.empty()) return;
                req = std::move(request_queue_.front());
                request_queue_.pop();
            }

            // Perform blocking DNS resolution on this dedicated thread
            struct addrinfo hints{};
            hints.ai_family   = AF_UNSPEC;      // IPv4 + IPv6
            hints.ai_socktype = SOCK_STREAM;     // TCP

            struct addrinfo* result = nullptr;
            int status = getaddrinfo(req.host.c_str(), req.port.c_str(),
                                     &hints, &result);

            DNSResult dns_result;
            dns_result.connection_id = req.connection_id;

            if (status == 0) {
                dns_result.addr_info = result;
            } else {
                dns_result.error = gai_strerror(status);
            }

            // Enqueue result and wake the event loop
            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                result_queue_.push(std::move(dns_result));
            }
            char byte = 1;
            ::write(notify_pipe_[1], &byte, 1);
        }
    }

public:
    /// @param num_threads  Number of worker threads for blocking DNS lookups.
    explicit DNSResolver(int num_threads = 2) {
        ::pipe(notify_pipe_);
        fcntl(notify_pipe_[0], F_SETFL, O_NONBLOCK);  // Non-blocking read end

        for (int i = 0; i < num_threads; i++) {
            workers_.emplace_back(&DNSResolver::worker_loop, this);
        }
    }

    ~DNSResolver() {
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            stop_ = true;
        }
        request_cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        if (notify_pipe_[0] >= 0) ::close(notify_pipe_[0]);
        if (notify_pipe_[1] >= 0) ::close(notify_pipe_[1]);
    }

    DNSResolver(const DNSResolver&) = delete;
    DNSResolver& operator=(const DNSResolver&) = delete;

    /// fd to register in the reactor's event loop for EV_READABLE.
    int notify_fd() const { return notify_pipe_[0]; }

    /// Submit a hostname for async resolution.
    void submit(int connection_id,
                const std::string& host, const std::string& port) {
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            request_queue_.push({connection_id, host, port});
        }
        request_cv_.notify_one();
    }

    /// Drain all completed results. Call from the event loop when notify_fd
    /// is readable. Returns a (possibly empty) batch.
    std::vector<DNSResult> drain() {
        // Drain the notification pipe (consume all pending wake-up bytes)
        char buf[256];
        while (::read(notify_pipe_[0], buf, sizeof(buf)) > 0) {}

        std::vector<DNSResult> results;
        std::lock_guard<std::mutex> lock(result_mutex_);
        while (!result_queue_.empty()) {
            results.push_back(std::move(result_queue_.front()));
            result_queue_.pop();
        }
        return results;
    }
};
