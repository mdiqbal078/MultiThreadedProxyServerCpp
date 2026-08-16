/*
 * event_loop.hpp -- Cross-platform event loop abstraction.
 *
 * Uses epoll on Linux and kqueue on macOS/BSD.
 * Level-triggered mode for correctness; upgrade to edge-triggered later
 * for a measurable "before/after" CV improvement.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <unistd.h>

#ifdef __APPLE__
  #include <sys/event.h>
#else
  #include <sys/epoll.h>
#endif

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
#ifdef __APPLE__
        poll_fd_ = kqueue();
#else
        poll_fd_ = epoll_create1(0);
#endif
    }

    ~EventLoop() {
        if (poll_fd_ >= 0) ::close(poll_fd_);
    }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool add(int fd, uint32_t interests) {
#ifdef __APPLE__
        struct kevent changes[2];
        int n = 0;
        if (interests & EV_READABLE)
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (interests & EV_WRITABLE)
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        return n > 0 && kevent(poll_fd_, changes, n, nullptr, 0, nullptr) >= 0;
#else
        struct epoll_event ev{};
        if (interests & EV_READABLE) ev.events |= EPOLLIN;
        if (interests & EV_WRITABLE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
#endif
    }

    bool modify(int fd, uint32_t interests) {
#ifdef __APPLE__
        // kqueue: add wanted filters, delete unwanted ones.
        // Deleting a non-existent filter returns ENOENT — we ignore it.
        struct kevent changes[4];
        int n = 0;
        if (interests & EV_READABLE)
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        else
            EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        if (interests & EV_WRITABLE)
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        else
            EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(poll_fd_, changes, n, nullptr, 0, nullptr);  // Ignore errors
        return true;
#else
        struct epoll_event ev{};
        if (interests & EV_READABLE) ev.events |= EPOLLIN;
        if (interests & EV_WRITABLE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
#endif
    }

    bool remove(int fd) {
#ifdef __APPLE__
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(poll_fd_, changes, 2, nullptr, 0, nullptr);  // Ignore errors
        return true;
#else
        return epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#endif
    }

    /// Block up to timeout_ms. Returns number of unique fd events in `out`.
    int wait(std::vector<PollEvent>& out, int timeout_ms) {
        out.clear();

#ifdef __APPLE__
        struct kevent kevents[MAX_EVENTS];
        struct timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};

        int n = kevent(poll_fd_, nullptr, 0, kevents, MAX_EVENTS,
                       timeout_ms >= 0 ? &ts : nullptr);
        if (n < 0) return -1;

        // kqueue fires one event per filter — merge into one PollEvent per fd.
        std::unordered_map<int, size_t> fd_idx;
        for (int i = 0; i < n; i++) {
            int fd = static_cast<int>(kevents[i].ident);
            uint32_t ev = 0;
            if (kevents[i].filter == EVFILT_READ)  ev |= EV_READABLE;
            if (kevents[i].filter == EVFILT_WRITE) ev |= EV_WRITABLE;
            if (kevents[i].flags & EV_EOF)         ev |= EV_READABLE;

            auto it = fd_idx.find(fd);
            if (it != fd_idx.end()) {
                out[it->second].events |= ev;
            } else {
                fd_idx[fd] = out.size();
                out.push_back({fd, ev});
            }
        }
        return static_cast<int>(out.size());

#else
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
#endif
    }
};
