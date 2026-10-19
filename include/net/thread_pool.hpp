#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace net {

// Bounded Thread Pool for offloading computation tasks to a fixed set of worker threads.
// Prevents thread explosion (the C10K problem) by keeping thread count bounded to hardware capacity.
class ThreadPool {
public:
    enum class OverflowPolicy {
        Block,  // Block submitting thread until space is available
        Reject  // Reject task immediately if queue is full
    };

    explicit ThreadPool(size_t num_threads, size_t max_queue_size = 1024,
                        OverflowPolicy policy = OverflowPolicy::Block)
        : max_queue_size_(max_queue_size), policy_(policy), stop_(false) {
        if (num_threads == 0) {
            num_threads = std::max(1u, std::thread::hardware_concurrency());
        }

        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this]() {
                worker_loop();
            });
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submits a callable task into the bounded queue
    bool submit(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) {
                return false;
            }

            if (tasks_.size() >= max_queue_size_) {
                if (policy_ == OverflowPolicy::Reject) {
                    return false; // Queue full, task rejected
                }
                // Wait for space in queue
                cv_space_.wait(lock, [this]() {
                    return stop_ || tasks_.size() < max_queue_size_;
                });
                if (stop_) return false;
            }

            tasks_.push(std::move(task));
        }
        cv_tasks_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) return;
            stop_ = true;
        }
        cv_tasks_.notify_all();
        cv_space_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    [[nodiscard]] size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    [[nodiscard]] size_t worker_count() const {
        return workers_.size();
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_tasks_.wait(lock, [this]() {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }
            cv_space_.notify_one();

            if (task) {
                task();
            }
        }
    }

    size_t max_queue_size_;
    OverflowPolicy policy_;
    bool stop_;

    mutable std::mutex mutex_;
    std::condition_variable cv_tasks_;
    std::condition_variable cv_space_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
};

} // namespace net

