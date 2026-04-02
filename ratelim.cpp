#include <iostream>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <memory>
#include <algorithm>
#include <thread>
#include <iomanip>

/**
 * A thread-safe Token Bucket Rate Limiter for a single user/entity.
 */
class TokenBucketRateLimiter {
private:
    const long capacity_;
    const double refill_rate_per_ns_;
    
    double current_tokens_;
    // EXTREMELY IMPORTANT: Use steady_clock, not system_clock.
    std::chrono::steady_clock::time_point last_refill_time_;
    
    // Mutex to protect this specific bucket's state
    std::mutex bucket_mtx_;

public:
    TokenBucketRateLimiter(long capacity, long refill_tokens, std::chrono::nanoseconds refill_period)
        : capacity_(capacity),
          refill_rate_per_ns_(static_cast<double>(refill_tokens) / refill_period.count()),
          current_tokens_(capacity),
          last_refill_time_(std::chrono::steady_clock::now()) {}

    bool tryConsume(int tokens_requested, double& out_tokens) {
        // lock_guard automatically unlocks when it goes out of scope
        std::lock_guard<std::mutex> lock(bucket_mtx_);
        
        refill();
        
        if (current_tokens_ >= tokens_requested) {
            current_tokens_ -= tokens_requested;
            out_tokens = current_tokens_;
            return true;
        }
        out_tokens = current_tokens_;
        return false;
    }

private:
    /**
     * LAZY REFILL: O(1) math operation calculated only when a request arrives.
     * Prevents needing a background thread per user.
     */
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto nanos_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_refill_time_).count();
        double tokens_to_add = nanos_elapsed * refill_rate_per_ns_;

        if (tokens_to_add > 0) {
            current_tokens_ = std::min(static_cast<double>(capacity_), current_tokens_ + tokens_to_add);
            last_refill_time_ = now;
        }
    }
};

/**
 * The API Gateway Manager that handles multiple clients concurrently.
 */
class RateLimiterManager {
private:
    // unordered_map is not thread-safe, so we need a lock for insertions
    std::unordered_map<std::string, std::shared_ptr<TokenBucketRateLimiter>> client_limiters_;
    std::mutex map_mtx_;
    
    long capacity_;
    long refill_tokens_;
    std::chrono::nanoseconds refill_period_;

public:
    RateLimiterManager(long capacity, long refill_tokens, std::chrono::nanoseconds refill_period)
        : capacity_(capacity), refill_tokens_(refill_tokens), refill_period_(refill_period) {}

    bool allowRequest(const std::string& client_id, double& out_tokens) {
        std::shared_ptr<TokenBucketRateLimiter> limiter;
        
        // CRITICAL SECTION 1: Map Lookup/Insertion
        {
            // We scope this lock artificially with { } so it releases immediately after lookup
            std::lock_guard<std::mutex> map_lock(map_mtx_);
            auto it = client_limiters_.find(client_id);
            if (it == client_limiters_.end()) {
                limiter = std::make_shared<TokenBucketRateLimiter>(capacity_, refill_tokens_, refill_period_);
                client_limiters_[client_id] = limiter;
            } else {
                limiter = it->second;
            }
        } // map_lock is destroyed and unlocked here!

        // CRITICAL SECTION 2: Token Consumption
        // We evaluate the bucket outside the map lock so User A doesn't block User B.
        return limiter->tryConsume(1, out_tokens);
    }
};

// Function to get current time string (similar to Python's time.strftime)
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt = *std::localtime(&timer);
    
    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

int main() {
    // Configuration: 5 tokens max, refills at 1 token per second
    const long capacity = 5;
    const long refill_tokens = 1;
    const auto refill_period = std::chrono::seconds(1);
    
    RateLimiterManager manager(capacity, refill_tokens, refill_period);
    
    std::string user = "user_123";
    
    std::cout << "Starting Rate Limiter Demo (C++)" << std::endl;
    std::cout << "Capacity: " << capacity << ", Refill: " << refill_tokens << " token(s) per " 
              << std::chrono::duration_cast<std::chrono::seconds>(refill_period).count() << "s" << std::endl;

    for (int i = 0; i < 20; ++i) {
        double current_tokens;
        bool allowed = manager.allowRequest(user, current_tokens);
        
        std::string time_str = get_timestamp();
        
        if (allowed) {
            std::cout << "[" << time_str << "] Request " << (i + 1) << ": Allowed (Tokens: " 
                      << std::fixed << std::setprecision(2) << current_tokens << ")" << std::endl;
        } else {
            std::cout << "[" << time_str << "] Request " << (i + 1) << ": Rate Limited (429) (Tokens: " 
                      << std::fixed << std::setprecision(2) << current_tokens << ")" << std::endl;
        }
        
        // Sleep for 200ms to trigger the limit (similar to Python script)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return 0;
}
