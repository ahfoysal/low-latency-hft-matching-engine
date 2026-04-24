// Synthetic tick generator. Produces a reproducible sequence of L2-like
// events (Add/Cancel) plus Trade prints around a mean-reverting-with-drift
// mid, written to a CSV file. Runs 100k ticks by default.
//
// Usage:
//   ./gen_ticks <out.csv> [num_ticks] [seed]

#include "backtest/replay.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using namespace hft;
using namespace hft::backtest;

int main(int argc, char** argv) {
    std::string out_path = "ticks.csv";
    size_t      n        = 100'000;
    uint64_t    seed     = 42;
    if (argc >= 2) out_path = argv[1];
    if (argc >= 3) n       = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    if (argc >= 4) seed    = std::strtoull(argv[3], nullptr, 10);

    std::mt19937_64               rng(seed);
    std::uniform_int_distribution<int> ev_dist(0, 99);        // pick event type
    std::uniform_int_distribution<int> side_dist(0, 1);       // buy/sell
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::normal_distribution<double>   drift_noise(0.0, 0.4); // mid-drift per tick
    std::uniform_int_distribution<int> depth_dist(1, 5);      // levels off best

    std::vector<Tick> ticks;
    ticks.reserve(n);

    // Start mid at 10000 ticks. Random walk with a weak mean reversion toward
    // 10000 so the book stays in a sane range for backtesting.
    double  mid       = 10000.0;
    const double anchor = 10000.0;
    uint64_t ts       = 1'000'000'000ull; // 1s into "epoch"
    uint64_t next_ext = 1;

    // Seed a couple hundred resting orders on each side so the book is
    // two-sided before any strategy arrives.
    for (int i = 0; i < 200; ++i) {
        Tick tA{};
        tA.ts_ns  = ts++;
        tA.symbol = 0;
        tA.event  = static_cast<uint8_t>(EventType::Add);
        tA.side   = 0; // Buy
        tA.price  = static_cast<int64_t>(mid) - 1 - (i % 10);
        tA.qty    = qty_dist(rng);
        tA.ext_id = next_ext++;
        ticks.push_back(tA);

        Tick tS = tA;
        tS.ts_ns  = ts++;
        tS.side   = 1;
        tS.price  = static_cast<int64_t>(mid) + 1 + (i % 10);
        tS.ext_id = next_ext++;
        ticks.push_back(tS);
    }

    // Main stream.
    // Distribution: ~70% Add, ~20% Trade prints, ~10% Cancel.
    std::vector<uint64_t> live_ext_ids;
    live_ext_ids.reserve(1 << 14);

    for (size_t i = 0; i < n; ++i) {
        // Gap between ticks: 100ns .. 10us, log-uniform.
        uint64_t gap_ns = static_cast<uint64_t>(100.0 *
            std::pow(100.0, std::uniform_real_distribution<double>(0.0, 1.0)(rng)));
        ts += gap_ns;

        // Mean-reverting random walk on mid.
        mid += drift_noise(rng) + 0.01 * (anchor - mid);

        Tick t{};
        t.ts_ns  = ts;
        t.symbol = 0;

        int roll = ev_dist(rng);
        if (roll < 70) {
            t.event = static_cast<uint8_t>(EventType::Add);
            int s = side_dist(rng);
            t.side = static_cast<uint8_t>(s);
            int off = depth_dist(rng);
            // ~15% of Adds are marketable (cross the spread) to generate
            // fills and trade activity.
            bool marketable = (ev_dist(rng) < 15);
            if (marketable) {
                t.price = static_cast<int64_t>(mid) + (s == 0 ? +off : -off);
            } else {
                t.price = static_cast<int64_t>(mid) + (s == 0 ? -off : +off);
            }
            t.qty    = qty_dist(rng);
            t.ext_id = next_ext++;
            live_ext_ids.push_back(t.ext_id);
        } else if (roll < 90) {
            t.event = static_cast<uint8_t>(EventType::Trade);
            t.side  = static_cast<uint8_t>(side_dist(rng));
            t.price = static_cast<int64_t>(mid);
            t.qty   = qty_dist(rng);
            t.ext_id = 0;
        } else {
            // Cancel a random live external id (if any).
            if (live_ext_ids.empty()) {
                --i; continue;
            }
            std::uniform_int_distribution<size_t> pick(0, live_ext_ids.size() - 1);
            size_t idx = pick(rng);
            t.event  = static_cast<uint8_t>(EventType::Cancel);
            t.ext_id = live_ext_ids[idx];
            live_ext_ids[idx] = live_ext_ids.back();
            live_ext_ids.pop_back();
        }

        ticks.push_back(t);
    }

    save_csv(out_path, ticks);
    std::printf("wrote %zu ticks to %s\n", ticks.size(), out_path.c_str());
    return 0;
}
