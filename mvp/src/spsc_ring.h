#pragma once

// Single-producer / single-consumer bounded ring buffer.
//
// Lock-free, wait-free on both sides. One cache line per index to avoid
// false sharing between producer and consumer. Capacity is rounded up to a
// power of two so index-wrapping is a bitmask instead of a modulo.
//
// Memory ordering follows the classic Vyukov SPSC recipe:
//   - Producer: read head_ (acquire), write slot, store head_+1 (release).
//   - Consumer: read tail_ (acquire), read slot, store tail_+1 (release).
// That pair forms a release/acquire synchronizes-with edge on the slot bytes.
//
// M2 uses this between a (future) order-ingress thread and the matching
// thread, but the order book itself remains single-threaded — the ring is
// the one crossing point, and it never allocates.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace hft {

namespace detail {
// 64 on most x86/arm64 desktops. C++17's hardware_destructive_interference_size
// is still a "maybe" on some libc++ builds, so we pin a concrete value rather
// than rely on an implementation-defined constexpr.
constexpr std::size_t kCacheLine = 64;

constexpr std::size_t round_up_pow2(std::size_t n) {
    if (n < 2) return 2;
    return std::bit_ceil(n);
}
} // namespace detail

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing payload must be trivially copyable for lock-free "
                  "hand-off without placement-new dance");
    static constexpr std::size_t kCap = detail::round_up_pow2(Capacity);
    static constexpr std::size_t kMask = kCap - 1;

public:
    SpscRing() : head_(0), tail_(0) {}

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer side. Returns false iff the ring is full (caller backs off or
    // drops — in HFT we size the ring so this never happens on the fast path).
    bool try_push(const T& v) noexcept {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = h + 1;
        // Full when producing next would collide with the consumer's tail.
        if (next - tail_.load(std::memory_order_acquire) > kCap) [[unlikely]] {
            return false;
        }
        buf_[h & kMask] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false iff the ring is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;
        }
        out = buf_[t & kMask];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Approximate size (consumer's view). Useful for metrics; do not use for
    // correctness decisions.
    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return h - t;
    }

    static constexpr std::size_t capacity() noexcept { return kCap; }

private:
    // head_ and tail_ are the only mutable state; isolate each on its own
    // cache line so the producer writing head_ doesn't evict the consumer's
    // cached tail_ and vice versa.
    alignas(detail::kCacheLine) std::atomic<std::size_t> head_;
    alignas(detail::kCacheLine) std::atomic<std::size_t> tail_;
    alignas(detail::kCacheLine) std::array<T, kCap>      buf_;
};

} // namespace hft
