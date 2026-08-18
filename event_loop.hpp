/*
 * event_loop.hpp -- Pure epoll event loop abstraction.
 *
 * Uses epoll on Linux.
 * Level-triggered mode for correctness.
 */

#pragma once

#include <vector>
#include <cstdint>
#include <unistd.h>
#include <sys/epoll.h>

// Interest flags
constexpr uint32_t EV_READABLE = 0x01;
constexpr uint32_t EV_WRITABLE = 0x02;

struct PollEvent {
    int fd;
    uint32_t events;  // Bitmask of EV_READABLE | EV_WRITABLE
};

class EventLoop {
    int poll_fd_;
    static constexpr int MAX_EVENTS = 1024;

public:
    EventLoop() {
        poll_fd_ = epoll_create1(0);
    }

    ~EventLoop() {
        if (poll_fd_ >= 0) ::close(poll_fd_);
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool add(int fd, uint32_t interests) {
        struct epoll_event ev{};
        if (interests & EV_READABLE) ev.events |= EPOLLIN;
        if (interests & EV_WRITABLE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool modify(int fd, uint32_t interests) {
        struct epoll_event ev{};
        if (interests & EV_READABLE) ev.events |= EPOLLIN;
        if (interests & EV_WRITABLE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    bool remove(int fd) {
        return epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    /// Block up to timeout_ms. Returns number of unique fd events in `out`.
    int wait(std::vector<PollEvent>& out, int timeout_ms) {
        out.clear();

        struct epoll_event epevents[MAX_EVENTS];
        int n = epoll_wait(poll_fd_, epevents, MAX_EVENTS, timeout_ms);
        if (n < 0) return -1;

        for (int i = 0; i < n; i++) {
            uint32_t ev = 0;
            if (epevents[i].events & EPOLLIN)               ev |= EV_READABLE;
            if (epevents[i].events & EPOLLOUT)              ev |= EV_WRITABLE;
            if (epevents[i].events & (EPOLLERR | EPOLLHUP)) ev |= EV_READABLE;
            out.push_back({epevents[i].data.fd, ev});
        }
        return n;
    }
};
