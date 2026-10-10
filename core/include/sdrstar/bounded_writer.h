// SPDX-License-Identifier: GPL-3.0-or-later
//
// BoundedWriter: feeds a child's stdin from a real-time thread without ever blocking it.  Data that the child cannot
// take is queued up to a limit; beyond that the OLDEST data is dropped (in whole `unit`-byte samples).
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "sdrstar/subprocess.h"

namespace sdrstar {

class BoundedWriter {
public:
    explicit BoundedWriter(Subprocess* p = nullptr, size_t capBytes = 262144, size_t unit = 2) : p_(p), cap_(capBytes), unit_(unit) {}
    void setProcess(Subprocess* p) { p_ = p; }
    // `unit` = bytes per sample (2 for s16 PCM, 8 for complex float): drops always remove whole samples.
    void configure(size_t capBytes, size_t unit) {
        std::lock_guard<std::mutex> lk(mtx_);
        cap_ = capBytes;
        unit_ = unit ? unit : 1;
        pending_.clear();
        dropped_ = 0;
    }

    void write(const void* data, size_t len) {
        if (!p_ || !p_->running() || len == 0) { return; }
        std::lock_guard<std::mutex> lk(mtx_);
        const uint8_t* b = (const uint8_t*)data;
        pending_.insert(pending_.end(), b, b + len);
        long w = p_->writeStdin(pending_.data(), pending_.size());
        if (w > 0) { pending_.erase(pending_.begin(), pending_.begin() + w); }
        if (pending_.size() > cap_) {
            size_t excess = ((pending_.size() - cap_) / unit_) * unit_;
            pending_.erase(pending_.begin(), pending_.begin() + (long)excess);
            dropped_ += (long long)excess;
        }
        // A partial write may split a sample; keep the stream sample-aligned by trimming nothing here: the child
        // reads a byte stream and the pending bytes are always resent in order, so alignment is preserved.
    }
    long long dropped() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mtx_));
        return dropped_;
    }

private:
    Subprocess* p_;
    size_t cap_;
    size_t unit_;
    std::mutex mtx_;
    std::vector<uint8_t> pending_;
    long long dropped_ = 0;
};

}  // namespace sdrstar
