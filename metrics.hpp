/*
 * metrics.hpp -- Atomic request-level counters for live observability.
 *
 * All counters are lock-free atomic<uint64_t>, safe to increment from
 * any reactor thread without synchronization overhead.
 */

#pragma once

#include <atomic>
#include <string>
#include <sstream>
#include <cstdint>

struct Metrics {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> active_connections{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> blocked_by_acl{0};
    std::atomic<uint64_t> rate_limited{0};
    std::atomic<uint64_t> connect_tunnels{0};

    double cache_hit_ratio() const {
        uint64_t h = cache_hits.load(std::memory_order_relaxed);
        uint64_t m = cache_misses.load(std::memory_order_relaxed);
        uint64_t total = h + m;
        return total > 0 ? (100.0 * static_cast<double>(h) / static_cast<double>(total)) : 0.0;
    }

    // Build a JSON snapshot. Accepts optional eviction stats from the cache.
    std::string to_json(uint64_t total_evictions = 0,
                        double avg_eviction_us = 0.0) const {
        std::ostringstream o;
        o << "{\n"
          << "  \"total_requests\": "       << total_requests.load()      << ",\n"
          << "  \"active_connections\": "    << active_connections.load()  << ",\n"
          << "  \"cache_hits\": "           << cache_hits.load()          << ",\n"
          << "  \"cache_misses\": "         << cache_misses.load()        << ",\n"
          << "  \"cache_hit_ratio_pct\": "  << cache_hit_ratio()          << ",\n"
          << "  \"blocked_by_acl\": "       << blocked_by_acl.load()      << ",\n"
          << "  \"rate_limited\": "         << rate_limited.load()        << ",\n"
          << "  \"connect_tunnels\": "      << connect_tunnels.load()     << ",\n"
          << "  \"total_evictions\": "      << total_evictions            << ",\n"
          << "  \"avg_eviction_latency_us\": " << avg_eviction_us         << "\n"
          << "}";
        return o.str();
    }
};
