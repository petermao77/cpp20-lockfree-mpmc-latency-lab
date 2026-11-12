/**
 * @file lockfree_mpmc_queue.hpp
 * @brief High-Performance Lock-Free Bounded MPMC Queue (C++20)
 *
 * Key Hardware & Concurrency Optimizations:
 * 1. Eliminates False Sharing via strict 64-byte cache-line isolation (`alignas(64)`).
 * 2. Enforces deterministic happens-before relationships using `std::memory_order_acquire`
 *    and `std::memory_order_release`, eliminating out-of-order SIGSEGV hazards.
 * 3. Zero dynamic allocations on the hot path (pre-allocated ring buffer).
 * 4. Power-of-two capacity enforced via C++20 concepts for fast bitwise index masking.
 */

#pragma once

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace latency_lab {

// Hardware L1/L2 cache line size (64 bytes on modern x86_64 & ARM64)
inline constexpr std::size_t kCacheLineSize = 64;

template <typename T>
concept LockFreeElement = std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>;

/**
 * @class LockFreeMPMCQueue
 * @brief Cache-line isolated, linearizable Multi-Producer Multi-Consumer ring buffer.
 * @tparam T Element type stored in the queue.
 */
template <LockFreeElement T>
class LockFreeMPMCQueue {
private:
    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::size_t> sequence{0};
        alignas(T) std::byte storage[sizeof(T)];

        Slot() noexcept = default;
        ~Slot() noexcept = default;

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };

public:
    /**
     * @brief Constructs a bounded MPMC queue.
     * @param capacity Must be a power of two and >= 2.
     */
    explicit LockFreeMPMCQueue(std::size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          buffer_(std::make_unique<Slot[]>(capacity)) {
        if (capacity < 2 || !std::has_single_bit(capacity)) {
            throw std::invalid_argument("Queue capacity must be a power of two >= 2.");
        }

        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~LockFreeMPMCQueue() noexcept {
        // Drain remaining elements safely upon destruction
        while (try_pop().has_value()) {}
    }

    LockFreeMPMCQueue(const LockFreeMPMCQueue&) = delete;
    LockFreeMPMCQueue& operator=(const LockFreeMPMCQueue&) = delete;

    /**
     * @brief Attempts to enqueue an element without blocking.
     * @return true if enqueued successfully, false if the queue is full.
     */
    template <typename... Args>
    requires std::constructible_from<T, Args...>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        Slot* slot = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &buffer_[pos & mask_];
            const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);

            if (diff == 0) {
                // Slot is free; attempt to claim it atomically
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                // Queue is full
                return false;
            } else {
                // Another producer claimed this slot; reload position
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        // Construct payload in-place inside isolated cache line
        std::construct_at(reinterpret_cast<T*>(slot->storage), std::forward<Args>(args)...);

        // Publish slot to consumers with release semantics (prevents store reordering)
        slot->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Attempts to dequeue an element without blocking.
     * @return std::optional<T> containing the element, or std::nullopt if empty.
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        Slot* slot = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &buffer_[pos & mask_];
            const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);

            if (diff == 0) {
                // Slot is populated; attempt to claim it atomically
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                // Queue is empty
                return std::nullopt;
            } else {
                // Another consumer claimed this slot; reload position
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T* elem_ptr = std::launder(reinterpret_cast<T*>(slot->storage));
        std::optional<T> result(std::move(*elem_ptr));
        std::destroy_at(elem_ptr);

        // Mark slot as available for the next wrap-around cycle
        slot->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return result;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Slot[]> buffer_;

    // Isolate producer and consumer cursors onto separate 64-byte cache lines
    // to eliminate the 16-byte False Sharing bottleneck.
    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
};

} // namespace latency_lab
