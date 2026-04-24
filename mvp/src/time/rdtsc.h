#pragma once

// Hardware-level nanosecond timer for the matching hot path.
//
// std::chrono::steady_clock on most libc++/glibc builds is backed by
// clock_gettime(CLOCK_MONOTONIC) which either goes through a vDSO (cheap,
// ~20-30ns on Linux) or a real syscall (macOS, ~40-60ns). At sub-microsecond
// latencies this timer is the dominant cost of "measuring" an op.
//
// We replace that with the CPU cycle counter:
//   - x86_64: RDTSC / RDTSCP (invariant TSC on all modern Intel/AMD)
//   - aarch64: CNTVCT_EL0 (ARM generic timer, cycle-count granularity)
//
// Both are ~5-10ns and don't leave user space. Calibration runs once at
// startup against a steady_clock anchor to convert ticks → nanoseconds.
//
// Usage:
//   auto t0 = hft::time::rdtsc();
//   ... hot path ...
//   auto t1 = hft::time::rdtsc();
//   uint64_t ns = hft::time::ticks_to_ns(t1 - t0);

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
  #include <x86intrin.h>
#endif

namespace hft::time {

// Read the hardware cycle counter.
inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    // RDTSC with serialization lfence to prevent reorder of surrounding loads.
    // On modern Intel the raw rdtsc is cheap (~15 cycles); lfence adds ~5.
    unsigned int aux;
    return __rdtscp(&aux);
#elif defined(__aarch64__)
    uint64_t v;
    // Virtual count register. Reads in EL0 are unprivileged on all modern
    // ARMv8 cores (Apple M-series, Graviton, Cortex-A7x, etc.).
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    // Fallback: steady_clock in ns (loses precision but keeps the API usable).
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// ns-per-tick calibration. Thread-safe after init (const after first call).
struct Calibration {
    double   ns_per_tick{1.0};
    uint64_t tick_at_anchor{0};
    uint64_t ns_at_anchor{0};
};

inline Calibration& calibration() {
    static Calibration c = [] {
        Calibration cc;
        using clk = std::chrono::steady_clock;

        // Warm-up: pull rdtsc + steady_clock into cache, let DVFS settle.
        for (int i = 0; i < 1000; ++i) { (void)rdtsc(); (void)clk::now(); }

        auto t0_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         clk::now().time_since_epoch()).count();
        uint64_t t0_tk = rdtsc();

        // 50ms window: enough for a stable ratio, short enough that startup
        // doesn't noticeably lag. 1ms gave us ±5% error on loaded M-series.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto t1_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         clk::now().time_since_epoch()).count();
        uint64_t t1_tk = rdtsc();

        uint64_t dt_ns = static_cast<uint64_t>(t1_ns - t0_ns);
        uint64_t dt_tk = t1_tk - t0_tk;
        cc.ns_per_tick  = (dt_tk > 0)
                            ? static_cast<double>(dt_ns) / static_cast<double>(dt_tk)
                            : 1.0;
        cc.tick_at_anchor = t0_tk;
        cc.ns_at_anchor   = static_cast<uint64_t>(t0_ns);
        return cc;
    }();
    return c;
}

// Force calibration before the hot path starts.
inline void init() { (void)calibration(); }

// Convert a tick *delta* to nanoseconds.
inline uint64_t ticks_to_ns(uint64_t ticks) noexcept {
    return static_cast<uint64_t>(
        static_cast<double>(ticks) * calibration().ns_per_tick + 0.5);
}

// Absolute wall-monotonic nanoseconds derived from rdtsc — cheaper than
// clock_gettime in the hot path but still anchored to steady_clock.
inline uint64_t now_ns() noexcept {
    const auto& c = calibration();
    uint64_t tk = rdtsc();
    uint64_t delta = tk - c.tick_at_anchor;
    return c.ns_at_anchor +
           static_cast<uint64_t>(static_cast<double>(delta) * c.ns_per_tick + 0.5);
}

} // namespace hft::time
