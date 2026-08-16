/*
 * lru_cache.hpp -- Thread-safe O(1) LRU Cache with eviction latency tracking.
 *
 * Uses std::list for LRU ordering and std::unordered_map for O(1) lookup.
 * Eviction latency is measured with high_resolution_clock so you can
 * report real microsecond numbers on your CV.
 */

#pragma once

#include <list>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <chrono>
#include <cstdint>

constexpr size_t DEFAULT_MAX_CACHE_SIZE    = 200ULL * (1 << 20);  // 200 MB
constexpr size_t DEFAULT_MAX_ELEMENT_SIZE  =  10ULL * (1 << 20);  //  10 MB

struct CacheEntry {
    std::string url;
    std::vector<char> data;
};

/// Tracks eviction count and cumulative latency for benchmarking.
struct EvictionStats {
    std::mutex mutex;
    uint64_t count = 0;
    double total_us = 0.0;

    void record(double microseconds) {
        std::lock_guard<std::mutex> lock(mutex);
        count++;
        total_us += microseconds;
    }

    uint64_t total_count() const { return count; }

    double average_us() const {
        return count > 0 ? total_us / static_cast<double>(count) : 0.0;
    }
};

class LRUCache {
    std::list<CacheEntry> lru_list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> map_;
    std::mutex mutex_;
    size_t current_size_ = 0;
    size_t max_size_;
    size_t max_element_size_;
    EvictionStats eviction_stats_;

    // Evict the least recently used entry (must be called under lock).
    void evict_one() {
        if (lru_list_.empty()) return;

        auto start = std::chrono::high_resolution_clock::now();

        auto last = std::prev(lru_list_.end());
        current_size_ -= (last->data.size() + last->url.size());
        map_.erase(last->url);
        lru_list_.pop_back();

        auto end = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(end - start).count();
        eviction_stats_.record(us);
    }

public:
    explicit LRUCache(size_t max_size    = DEFAULT_MAX_CACHE_SIZE,
                      size_t max_element = DEFAULT_MAX_ELEMENT_SIZE)
        : max_size_(max_size), max_element_size_(max_element) {}

    std::optional<std::vector<char>> get(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(url);
        if (it == map_.end()) return std::nullopt;

        // Promote to most recently used
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return it->second->data;
    }

    void put(const std::string& url, const std::vector<char>& data) {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t elem_size = data.size() + url.size();
        if (elem_size > max_element_size_) return;

        auto it = map_.find(url);
        if (it != map_.end()) {
            current_size_ -= it->second->data.size();
            it->second->data = data;
            current_size_ += data.size();
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        } else {
            while (current_size_ + elem_size > max_size_ && !lru_list_.empty()) {
                evict_one();
            }
            lru_list_.push_front({url, data});
            map_[url] = lru_list_.begin();
            current_size_ += elem_size;
        }
    }

    const EvictionStats& eviction_stats() const { return eviction_stats_; }
};
