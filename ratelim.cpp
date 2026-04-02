#include <iostream>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <memory>
#include <algorithm>
#include <thread>
#include <iomanip>
#include <atomic>

/**
 * Shared configuration for a specific tier (e.g., "Free", "Premium").
 * Using atomics allows global updates to be thread-safe and instant.
 */
struct RateLimitConfig {
    std::atomic<long> capacity;
    std::atomic<double> refill_rate_per_ns;

    RateLimitConfig(long cap, long tokens, std::chrono::nanoseconds period)
        : capacity(cap), 
          refill_rate_per_ns(static_cast<double>(tokens) / period.count()) {}
};

/**
 * A thread-safe Token Bucket Rate Limiter for a single user/entity.
 * State (tokens, last_refill) is private, but Config is shared.
 */
class TokenBucketRateLimiter {
private:
    std::shared_ptr<RateLimitConfig> config_;
    
    double current_tokens_;
    std::chrono::steady_clock::time_point last_refill_time_;
    std::mutex bucket_mtx_;

public:
    TokenBucketRateLimiter(std::shared_ptr<RateLimitConfig> config)
        : config_(config),
          current_tokens_(config->capacity.load()),
          last_refill_time_(std::chrono::steady_clock::now()) {}

    bool tryConsume(int tokens_requested, double &out_tokens) {
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
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto nanos_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 now - last_refill_time_)
                                 .count();
        
        // Load atomic values from shared config
        double rate = config_->refill_rate_per_ns.load();
        long cap = config_->capacity.load();

        double tokens_to_add = nanos_elapsed * rate;

        if (tokens_to_add > 0) {
            current_tokens_ = std::min(static_cast<double>(cap),
                                       current_tokens_ + tokens_to_add);
            last_refill_time_ = now;
        }
    }
};

/**
 * The API Gateway Manager.
 * Manages tiers (configs) and client buckets.
 */
class RateLimiterManager {
private:
    std::unordered_map<std::string, std::shared_ptr<TokenBucketRateLimiter>> client_limiters_;
    std::unordered_map<std::string, std::shared_ptr<RateLimitConfig>> tier_configs_;
    std::mutex manager_mtx_;

public:
    void createTier(const std::string& tier_name, long capacity, long tokens, std::chrono::nanoseconds period) {
        std::lock_guard<std::mutex> lock(manager_mtx_);
        tier_configs_[tier_name] = std::make_shared<RateLimitConfig>(capacity, tokens, period);
    }

    std::shared_ptr<RateLimitConfig> getTier(const std::string& tier_name) {
        std::lock_guard<std::mutex> lock(manager_mtx_);
        return tier_configs_[tier_name];
    }

    bool allowRequest(const std::string &client_id, const std::string& tier_name, double &out_tokens) {
        std::shared_ptr<TokenBucketRateLimiter> limiter;
        {
            std::lock_guard<std::mutex> map_lock(manager_mtx_);
            auto it = client_limiters_.find(client_id);
            if (it == client_limiters_.end()) {
                auto config_it = tier_configs_.find(tier_name);
                if (config_it == tier_configs_.end()) return false; // Tier must exist
                
                limiter = std::make_shared<TokenBucketRateLimiter>(config_it->second);
                client_limiters_[client_id] = limiter;
            } else {
                limiter = it->second;
            }
        } 
        return limiter->tryConsume(1, out_tokens);
    }
};

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt = *std::localtime(&timer);
    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

int main() {
    RateLimiterManager manager;
    
    // 1. Create a "Free Tier": 2 tokens capacity, refills 1 token per 2 seconds (0.5/sec)
    manager.createTier("Free", 2, 1, std::chrono::seconds(2));
    auto free_config = manager.getTier("Free");

    std::cout << "--- Initial State (Free Tier: Cap 2, Refill 0.5/sec) ---" << std::endl;
    
    auto run_test = [&](const std::string& user, int count) {
        for (int i = 0; i < count; ++i) {
            double tokens;
            bool allowed = manager.allowRequest(user, "Free", tokens);
            std::cout << "[" << get_timestamp() << "] [" << user << "] Request " << (i+1) 
                      << ": " << (allowed ? "Allowed" : "Limited") << " (Tokens: " << tokens << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    };

    // User A and B both share the same "Free" config object
    std::thread t1(run_test, "User A", 6);
    std::thread t2(run_test, "User B", 6);
    t1.join(); t2.join();

    std::cout << "\n--- GLOBAL UPDATE: Upgrading 'Free Tier' to Cap 5, Refill 10/sec ---" << std::endl;
    // Changing these atomic values instantly affects all buckets referencing this config
    free_config->capacity.store(5);
    free_config->refill_rate_per_ns.store(10.0 / 1e9); 

    std::thread t3(run_test, "User A", 6);
    std::thread t4(run_test, "User B", 6);
    t3.join(); t4.join();

    return 0;
}
