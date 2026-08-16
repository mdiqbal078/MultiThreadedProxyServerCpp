/*
 * acl.hpp -- Domain blocklist (Access Control List).
 *
 * Read-only after initialization, so no mutex is needed during
 * request processing. Case-insensitive hostname matching.
 */

#pragma once

#include <unordered_set>
#include <string>
#include <algorithm>

class ACL {
    std::unordered_set<std::string> blocklist_;

    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }

public:
    void block(const std::string& domain) {
        blocklist_.insert(to_lower(domain));
    }

    bool is_blocked(const std::string& domain) const {
        return blocklist_.count(to_lower(domain)) > 0;
    }

    size_t size() const { return blocklist_.size(); }
};
