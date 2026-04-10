// FrameQueue.h
// A bounded, thread-safe queue for passing GPU frame objects between threads.
// Producers call Push(); consumers call Pop() which blocks until an item or
// an interrupt signal arrives.  When Interrupt() is called all blocked waiters
// unblock and subsequent calls return std::nullopt.
#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

template<typename T>
class FrameQueue
{
public:
    explicit FrameQueue(size_t maxDepth = 4) : maxDepth_(maxDepth) {}

    // Push an item.  Blocks if the queue is full (back-pressure).
    // Returns false if the queue was interrupted before the push completed.
    bool Push(T item)
    {
        std::unique_lock lock(mu_);
        full_.wait(lock, [this]{ return q_.size() < maxDepth_ || interrupted_; });
        if (interrupted_) return false;
        q_.push(std::move(item));
        empty_.notify_one();
        return true;
    }

    // Pop an item.  Blocks until an item is available or Interrupt() is called.
    // Returns std::nullopt on interrupt.
    std::optional<T> Pop()
    {
        std::unique_lock lock(mu_);
        empty_.wait(lock, [this]{ return !q_.empty() || interrupted_; });
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop();
        full_.notify_one();
        return item;
    }

    // Unblock all waiters.  The queue is unusable after this.
    void Interrupt()
    {
        std::unique_lock lock(mu_);
        interrupted_ = true;
        empty_.notify_all();
        full_.notify_all();
    }

    void Reset()
    {
        std::unique_lock lock(mu_);
        while (!q_.empty()) q_.pop();
        interrupted_ = false;
    }

    size_t Size() const
    {
        std::unique_lock lock(mu_);
        return q_.size();
    }

private:
    mutable std::mutex      mu_;
    std::condition_variable empty_;
    std::condition_variable full_;
    std::queue<T>           q_;
    size_t                  maxDepth_;
    bool                    interrupted_ = false;
};
