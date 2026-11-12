/**
 * @file defective_queue.hpp
 * @brief [CASE STUDY: DEFECTIVE BASELINE] Legacy Ring Buffer with Hardware & Concurrency Flaws
 *
 * WARNING: DO NOT USE IN PRODUCTION.
 * This file intentionally reproduces two severe low-level systems defects for benchmarking
 * and ThreadSanitizer (TSan) diagnostic demonstrations:
 *
 * 1. 16-Byte False Sharing: `head_` and `tail_` (8 bytes each) are packed into the same
 *    64-byte CPU cache line, causing massive L1/L2 cache invalidation storms across cores
 *    (leading to ~180us p99 tail latency spikes and 0.35 IPC).
 * 2. Broken Memory Ordering: Relying on `std::memory_order_relaxed` for index publication
 *    allows CPU store/load reordering, exposing uninitialized pointers and triggering
 *    intermittent `SIGSEGV` crashes under burst load.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace latency_lab {

/**
 * @class DefectiveQueue
 * @brief Unaligned, memory-ordering-broken queue representing the pre-optimization baseline.
 */
template <typename T>
class DefectiveQueue {
private:
    // FLAW 1: Unaligned ControlBlock packs `head_` (8B) and `tail_` (8B) into the exact same
    // 64-byte cache line. Concurrent producer/consumer updates trigger constant cache-line
    // bouncing (16B False Sharing).
    struct ControlBlock {
        std::atomic<std::size_t> head_{0}; // 8 Bytes
        std::atomic<std::size_t> tail_{0}; // 8 Bytes -> Total 16B sharing one cache line
    };

public:
    explicit DefectiveQueue(std::size_t capacity)
        : capacity_(capacity),
          buffer_(std::make_unique<T*[]>(capacity)) {
        if (capacity < 2) {
            throw std::invalid_argument("Queue capacity must be >= 2.");
        }
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i] = nullptr;
        }
    }

    ~DefectiveQueue() noexcept {
        for (std::size_t i = 0; i < capacity_; ++i) {
            delete buffer_[i];
        }
    }

    DefectiveQueue(const DefectiveQueue&) = delete;
    DefectiveQueue& operator=(const DefectiveQueue&) = delete;

    /**
     * @brief Enqueues an item with broken memory ordering and heap allocation overhead.
     */
    [[nodiscard]] bool push(T value) {
        // FLAW 2: `memory_order_relaxed` provides NO synchronization barrier.
        const std::size_t current_tail = ctrl_.tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = (current_tail + 1) % capacity_;

        if (next_tail == ctrl_.head_.load(std::memory_order_relaxed)) {
            return false; // Queue appears full
        }

        // Heap allocation on hot path increases latency variance
        T* payload = new T(std::move(value));
        buffer_[current_tail] = payload;

        // CRITICAL BUG: Publishing `tail_` with `relaxed` allows the CPU/compiler to reorder
        // this store BEFORE `buffer_[current_tail] = payload` becomes visible to other cores!
        ctrl_.tail_.store(next_tail, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief Dequeues an item; vulnerable to reading stale/null pointers (SIGSEGV).
     */
    [[nodiscard]] std::optional<T> pop() {
        const std::size_t current_head = ctrl_.head_.load(std::memory_order_relaxed);

        // CRITICAL BUG: Reading `tail_` with `relaxed` may observe the updated index
        // before `buffer_[current_head]` is visible, causing a 0x0 nullptr dereference.
        if (current_head == ctrl_.tail_.load(std::memory_order_relaxed)) {
            return std::nullopt; // Queue appears empty
        }

        T* payload = buffer_[current_head];
        buffer_[current_head] = nullptr;

        // Potential SIGSEGV hazard if `payload` has not propagated across L1/L2 caches yet
        T result = std::move(*payload);
        delete payload;

        ctrl_.head_.store((current_head + 1) % capacity_, std::memory_order_relaxed);
        return result;
    }

private:
    const std::size_t capacity_;
    std::unique_ptr<T*[]> buffer_;
    ControlBlock ctrl_; // Contended 16-byte hotspot
};

} // namespace latency_lab
