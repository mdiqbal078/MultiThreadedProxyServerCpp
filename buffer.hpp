/*
 * buffer.hpp -- Non-blocking I/O buffer with zero-copy reads.
 *
 * Supports incremental append, partial consume, and string_view access
 * for scanning delimiters (\r\n\r\n) without copying data.
 */

#pragma once

#include <vector>
#include <string_view>
#include <cstring>
#include <algorithm>

class Buffer {
    std::vector<char> data_;
    size_t read_pos_ = 0;

public:
    void append(const char* src, size_t len) {
        data_.insert(data_.end(), src, src + len);
    }

    const char* data() const { return data_.data() + read_pos_; }
    size_t size() const { return data_.size() - read_pos_; }
    bool empty() const { return size() == 0; }

    void consume(size_t n) {
        read_pos_ = std::min(read_pos_ + n, data_.size());
        if (read_pos_ == data_.size()) {
            data_.clear();
            read_pos_ = 0;
        }
    }

    void compact() {
        if (read_pos_ > 0) {
            data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
            read_pos_ = 0;
        }
    }

    void clear() {
        data_.clear();
        read_pos_ = 0;
    }

    std::string_view view() const {
        return {data(), size()};
    }
};
