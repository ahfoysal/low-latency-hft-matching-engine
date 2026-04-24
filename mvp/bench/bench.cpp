// Microbenchmark: N limit-order placements, per-op latency distribution.
//
// M5 update: replace chrono::steady_clock with the RDTSC/CNTVCT timer from
// hft::time, and pin this thread to a single CPU (isolated core on Linux,
// single perf-core hint on macOS) so the measurement isn't polluted by
// scheduler migration or the ~40-60ns clock_gettime hop.
#include "engine.h"
#include "core/affinity.h"
#include "time/rdtsc.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace hft;

int main(int argc, char** argv) {
    const size_t N = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const int pin_cpu = (argc > 2) ? std::atoi(argv[2]) : 2;

    bool pinned = hft::core::pin_thread_to_cpu(pin_cpu);
    // Linux only — will silently fail without CAP_SYS_NICE.
    (void)hft::core::set_realtime_priority(90);

    // Force calibration before we start timing.
    hft::time::init();
    const double ns_per_tick = hft::time::calibration().ns_per_tick;

    Engine e;
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int> px_d(9990, 10010);
    std::uniform_int_distribution<int> qty_d(1, 10);
    std::uniform_int_distribution<int> side_d(0, 1);

    std::vector<uint64_t> lat_ticks;
    lat_ticks.reserve(N);

    // Warm up — populate book, prime TLB / I$ / D$.
    for (size_t i = 0; i < 10'000; ++i) {
        Side s = side_d(rng) ? Side::Buy : Side::Sell;
        e.place_limit(s, px_d(rng), qty_d(rng));
    }

    uint64_t wall_t0 = hft::time::now_ns();
    uint64_t tk_start = hft::time::rdtsc();
    for (size_t i = 0; i < N; ++i) {
        Side s  = side_d(rng) ? Side::Buy : Side::Sell;
        int  px = px_d(rng);
        int  q  = qty_d(rng);
        uint64_t a = hft::time::rdtsc();
        e.place_limit(s, px, q);
        uint64_t b = hft::time::rdtsc();
        lat_ticks.push_back(b - a);
    }
    uint64_t tk_end = hft::time::rdtsc();
    uint64_t wall_t1 = hft::time::now_ns();

    // Convert ticks → ns once, in bulk, so the multiply isn't on the hot path.
    std::vector<uint64_t> lat_ns;
    lat_ns.reserve(lat_ticks.size());
    for (uint64_t t : lat_ticks)
        lat_ns.push_back(static_cast<uint64_t>(static_cast<double>(t) * ns_per_tick + 0.5));

    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) { return lat_ns[(size_t)((lat_ns.size() - 1) * p)]; };
    uint64_t p50  = pct(0.50);
    uint64_t p90  = pct(0.90);
    uint64_t p99  = pct(0.99);
    uint64_t p999 = pct(0.999);
    uint64_t pmax = lat_ns.back();

    double elapsed_s = (wall_t1 - wall_t0) / 1e9;
    double ops_per_s = N / elapsed_s;
    double total_ns_ticks = static_cast<double>(tk_end - tk_start) * ns_per_tick;

    std::printf("=== MVP order book microbench (M5) ===\n");
    std::printf("timer      : %s\n",
#if defined(__x86_64__) || defined(_M_X64)
                "RDTSCP (x86)"
#elif defined(__aarch64__)
                "CNTVCT_EL0 (aarch64)"
#else
                "chrono fallback"
#endif
    );
    std::printf("pinned     : cpu=%d %s\n", pin_cpu, pinned ? "ok" : "hint-only/failed");
    std::printf("ns/tick    : %.4f\n", ns_per_tick);
    std::printf("operations : %zu place_limit (mixed buy/sell, crossing)\n", N);
    std::printf("elapsed    : %.3f s (rdtsc-window %.3f s)\n",
                elapsed_s, total_ns_ticks / 1e9);
    std::printf("throughput : %.0f ops/sec\n", ops_per_s);
    std::printf("latency ns : p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)p50, (unsigned long long)p90,
                (unsigned long long)p99, (unsigned long long)p999,
                (unsigned long long)pmax);
    std::printf("resting    : %zu orders on book\n", e.book().size());
    return 0;
}
