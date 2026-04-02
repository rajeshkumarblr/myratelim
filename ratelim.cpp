#include <iostream>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <memory>
#include <algorithm>
#include <thread>
#include <iomanip>
#include <vector>

/**
 * Capture the details of a consumption attempt for debugging/demonstration.
 */
struct ConsumptionResult {
    bool allowed;
    double tokens_before_refill;
    double tokens_after_refill;
    double tokens_after_consume;
    long long nanos_elapsed;
    double tokens_added;
};

/**
 * A thread-safe Token Bucket Rate Limiter for a single user/entity.
 */
class TokenBucketRateLimiter {
private:
    const long capacity_;
    const double refill_rate_per_ns_;
    
    double current_tokens_;
    std::chrono::steady_clock::time_point last_refill_time_;
    std::mutex bucket_mtx_;

public:
    TokenBucketRateLimiter(long capacity, long refill_tokens, std::chrono::nanoseconds refill_period)
        : capacity_(capacity),
          refill_rate_per_ns_(static_cast<double>(refill_tokens) / refill_period.count()),
          current_tokens_(capacity),
          last_refill_time_(std::chrono::steady_clock::now()) {}

    ConsumptionResult tryConsume(int tokens_requested) {
        std::lock_guard<std::mutex> lock(bucket_mtx_);
        
        ConsumptionResult res;
        res.tokens_before_refill = current_tokens_;
        
        auto now = std::chrono::steady_clock::now();
        res.nanos_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_refill_time_).count();
        res.tokens_added = res.nanos_elapsed * refill_rate_per_ns_;

        if (res.tokens_added > 0) {
            current_tokens_ = std::min(static_cast<double>(capacity_), current_tokens_ + res.tokens_added);
            last_refill_time_ = now;
        }
        
        res.tokens_after_refill = current_tokens_;
        
        if (current_tokens_ >= tokens_requested) {
            current_tokens_ -= tokens_requested;
            res.allowed = true;
        } else {
            res.allowed = false;
        }
        
        res.tokens_after_consume = current_tokens_;
        return res;
    }
};

/**
 * The API Gateway Manager that handles multiple clients concurrently.
 */
class RateLimiterManager {
private:
    std::unordered_map<std::string, std::shared_ptr<TokenBucketRateLimiter>> client_limiters_;
    std::mutex map_mtx_;
    
    long capacity_;
    long refill_tokens_;
    std::chrono::nanoseconds refill_period_;

public:
    RateLimiterManager(long capacity, long refill_tokens, std::chrono::nanoseconds refill_period)
        : capacity_(capacity), refill_tokens_(refill_tokens), refill_period_(refill_period) {}

    ConsumptionResult allowRequest(const std::string& client_id) {
        std::shared_ptr<TokenBucketRateLimiter> limiter;
        {
            std::lock_guard<std::mutex> map_lock(map_mtx_);
            auto it = client_limiters_.find(client_id);
            if (it == client_limiters_.end()) {
                limiter = std::make_shared<TokenBucketRateLimiter>(capacity_, refill_tokens_, refill_period_);
                client_limiters_[client_id] = limiter;
            } else {
                limiter = it->second;
            }
        }
        return limiter->tryConsume(1);
    }
};

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

// Global mutex for clean console output from multiple threads
std::mutex cout_mtx;

void simulate_user(RateLimiterManager& manager, std::string user_id, int requests, int sleep_ms) {
    for (int i = 0; i < requests; ++i) {
        auto res = manager.allowRequest(user_id);
        
        {
            std::lock_guard<std::mutex> lock(cout_mtx);
            std::cout << "[" << get_timestamp() << "] [" << user_id << "] Request " << std::setw(2) << (i + 1) << ": "
                      << (res.allowed ? "ALLOWED " : "LIMITED ")
                      << "| Math: " << std::fixed << std::setprecision(2) << res.tokens_before_refill 
                      << " + (" << res.nanos_elapsed << "ns * rate) -> " << res.tokens_after_refill 
                      << " | Result: " << res.tokens_after_consume << std::endl;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
}

int main() {
    const long capacity = 5;
    const long refill_tokens = 1;
    const auto refill_period = std::chrono::seconds(1);
    
    RateLimiterManager manager(capacity, refill_tokens, refill_period);
    
    std::cout << "Starting Enhanced Multi-User Rate Limiter Demo" << std::endl;
    std::cout << "Capacity: " << capacity << ", Refill: 1 token/sec" << std::endl;
    std::cout << "Running User A and User B concurrently with independent buckets..." << std::endl;
    std::cout << std::string(100, '-') << std::endl;

    // Launch two threads simulating different users
    // User A: Requests every 200ms
    // User B: Requests every 500ms
    std::thread t1(simulate_user, std::ref(manager), "User A", 15, 200);
    std::thread t2(simulate_user, std::ref(manager), "User B", 10, 500);

    t1.join();
    t2.join();

    std::cout << std::string(100, '-') << std::endl;
    std::cout << "Demo Completed." << std::endl;

    return 0;
}
