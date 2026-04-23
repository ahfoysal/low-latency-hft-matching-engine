// Microbenchmark: N limit-order placements + measures per-op latency
// distribution (p50/p99/p999) and throughput. No external bench framework
// dependency so the MVP builds with just clang + cmake.
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace hft;
using clk = std::chrono::steady_clock;

static inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clk::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    const size_t N = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;

    Engine e;
    std::mt19937_64 rng(0xC0FFEE);
    // Tight spread around 10000 to keep some crossing volume.
    std::uniform_int_distribution<int> px_d(9990, 10010);
    std::uniform_int_distribution<int> qty_d(1, 10);
    std::uniform_int_distribution<int> side_d(0, 1);

    std::vector<uint64_t> lat;
    lat.reserve(N);

    // Warm up.
    for (size_t i = 0; i < 10'000; ++i) {
        Side s = side_d(rng) ? Side::Buy : Side::Sell;
        e.place_limit(s, px_d(rng), qty_d(rng));
    }

    uint64_t t0 = now_ns();
    for (size_t i = 0; i < N; ++i) {
        Side s  = side_d(rng) ? Side::Buy : Side::Sell;
        int  px = px_d(rng);
        int  q  = qty_d(rng);
        uint64_t a = now_ns();
        e.place_limit(s, px, q);
        uint64_t b = now_ns();
        lat.push_back(b - a);
    }
    uint64_t t1 = now_ns();

    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[(size_t)((lat.size() - 1) * p)]; };
    uint64_t p50  = pct(0.50);
    uint64_t p90  = pct(0.90);
    uint64_t p99  = pct(0.99);
    uint64_t p999 = pct(0.999);
    uint64_t pmax = lat.back();
    double elapsed_s = (t1 - t0) / 1e9;
    double ops_per_s = N / elapsed_s;

    std::printf("=== MVP order book microbench ===\n");
    std::printf("operations : %zu place_limit (mixed buy/sell, crossing)\n", N);
    std::printf("elapsed    : %.3f s\n", elapsed_s);
    std::printf("throughput : %.0f ops/sec\n", ops_per_s);
    std::printf("latency ns : p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu\n",
                (unsigned long long)p50, (unsigned long long)p90,
                (unsigned long long)p99, (unsigned long long)p999,
                (unsigned long long)pmax);
    std::printf("resting    : %zu orders on book\n", e.book().size());
    return 0;
}
