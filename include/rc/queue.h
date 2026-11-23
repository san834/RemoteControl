// 有界阻塞队列：捕获线程 → 发送线程、UI 线程 → 发送线程 的唯一通道。
//
// 用它替代旧实现的 PostThreadMessage：消息循环方案要求每个工作线程自建
// 消息队列，CreateThread 之后立刻 Post 会因为队列尚未创建而丢消息（旧代码
// 靠 Sleep(100) 兜底），而且没有任何背压能力。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace rc {

template <typename T>
class BlockingQueue {
public:
    // capacity == 0 表示不限长度。
    explicit BlockingQueue(std::size_t capacity = 0) : capacity_(capacity) {}

    // 队列满时丢弃新元素并返回 false（视频流水线里"丢帧"是正确行为）。
    bool Push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            if (capacity_ != 0 && queue_.size() >= capacity_) return false;
            queue_.push_back(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    // 阻塞直到有数据。队列被 Close 且已排空时返回 false。
    bool Pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // 带超时的 Pop，用于需要周期性检查退出标志的线程。
    bool PopFor(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout,
                                 [this] { return closed_ || !queue_.empty(); })) {
            return false;
        }
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    std::size_t capacity_;
    bool closed_ = false;
};

}  // namespace rc
