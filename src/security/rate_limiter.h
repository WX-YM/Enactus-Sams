#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <deque>

namespace enactus::security {

class SlidingWindowRateLimiter {
public:
    SlidingWindowRateLimiter(size_t max_requests, std::chrono::seconds window)
        : max_requests_(max_requests), window_(window) {}

    bool isAllowed(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - window_;

        auto& timestamps = entries_[key];
        while (!timestamps.empty() && timestamps.front() < cutoff) {
            timestamps.pop_front();
        }

        if (timestamps.size() >= max_requests_) {
            return false;
        }

        timestamps.push_back(now);

        // Periodically prune stale keys if map grows large (> 2000 entries)
        if (entries_.size() > 2000) {
            for (auto it = entries_.begin(); it != entries_.end(); ) {
                if (it->second.empty() || it->second.back() < cutoff) {
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        return true;
    }

private:
    std::mutex mutex_;
    size_t max_requests_;
    std::chrono::seconds window_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> entries_;
};

} // namespace enactus::security
