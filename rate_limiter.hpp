/*
 * rate_limiter.hpp -- Thread-safe per-IP token bucket rate limiter.
 *
 * Each client IP gets its own bucket. Tokens refill at a constant rate.
 * When a bucket is empty, the request is rejected with 429.
 */

#pragma once

#include <unordered_map>
#include <chrono>
#include <mutex>
#include <string>

class RateLimiter {
    struct TokenBucket {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;

        TokenBucket()
            : tokens(0), last_refill(std::chrono::steady_clock::time_point{}) {}
    };

    double max_tokens_;
    double refill_rate_;  // tokens per second

    std::unordered_map<std::string, TokenBucket> buckets_;
    std::mutex mutex_;

public:
    /// @param max_tokens  Maximum burst size per IP.
    /// @param refill_rate Tokens added per second.
    explicit RateLimiter(double max_tokens = 20.0, double refill_rate = 10.0)
        : max_tokens_(max_tokens), refill_rate_(refill_rate) {}

    /// Returns true if the request from this IP is allowed.
    bool allow(const std::string& client_ip) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();
        auto& bucket = buckets_[client_ip];

        // Initialize new bucket
        if (bucket.last_refill.time_since_epoch().count() == 0) {
            bucket.tokens = max_tokens_;
            bucket.last_refill = now;
        }

        // Refill tokens based on elapsed time
        double elapsed =
            std::chrono::duration<double>(now - bucket.last_refill).count();
        bucket.tokens = std::min(max_tokens_, bucket.tokens + elapsed * refill_rate_);
        bucket.last_refill = now;

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;  // Rate limited
    }
};
