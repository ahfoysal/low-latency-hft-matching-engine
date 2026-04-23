# HFT Trading Engine

**Stack:** C++20 (core) · Rust (gateways/tools) · LMAX Disruptor-style ring buffers · io_uring (later DPDK) · FIX 4.4 + SBE binary protocol · Google Benchmark · perf/flamegraph · Linux isolcpus + HUGEPAGES

## Full Vision
Sub-microsecond matching engine, kernel bypass, colocated deploy, PTP nanosecond clock sync, backtester with L3 tick data, risk engine, strategy DSL.

## MVP (1 weekend)
In-memory limit order book with `place/cancel/match` — target <10μs p99 single-threaded.

## MVP Status — shipped

Single-threaded limit order book with price-time priority, matching on placement, and cancel. Lives under [`mvp/`](./mvp).

- **M1:** bid/ask sides are `std::map<Price, std::deque<Order>>`.
- **M2:** each price level is an intrusive doubly-linked list of `OrderNode`s drawn from a preallocated `ObjectPool` — zero heap allocations on the hot path. Cancel drops from O(level) to O(1). Adds an SPSC ring ([`mvp/src/spsc_ring.h`](./mvp/src/spsc_ring.h)) for lock-free producer→matcher hand-off (wired into the sequencer in a later milestone).

### Build

```bash
cd mvp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/test_book          # unit tests
./build/bench 1000000      # microbenchmark
```

### Tests (6/6 passing)

- `simple_buy_rests` — non-crossing buy rests as best bid
- `simple_sell_rests` — non-crossing sell rests as best ask
- `partial_fill` — crossing buy partially fills then rests the remainder
- `full_cross_multiple_levels` — taker walks 2 levels, remainder posts
- `cancel` — cancel removes order, empty level is pruned, double-cancel fails
- `price_time_priority_fifo` — earlier order at same price fills first

### Benchmark (Apple M-series, macOS arm64, clang 17, -O3)

1,000,000 `place_limit` calls, mixed buy/sell, prices uniform in [9990, 10010] so ~half cross:

| metric        | M1 (deque + heap)             | M2 (intrusive + pool)       | Δ             |
|---------------|-------------------------------|-----------------------------|---------------|
| throughput    | 5,599,824 ops/s               | 8,158,440 ops/s             | **+46%**      |
| latency p50   | 84 ns                         | 83 ns                       | flat          |
| latency p90   | 250 ns                        | 125 ns                      | **−2x**       |
| latency p99   | 500 ns                        | 250 ns                      | **−2x**       |
| latency p99.9 | 1458 ns                       | ~1200 ns                    | **−18%**      |
| latency max   | 1.80 ms (heap / page fault)   | ~75–100 μs                  | **~20× lower**|

The big win is the max: killing the per-order `std::deque<Order>` heap allocation flattens the worst-case tail by roughly an order of magnitude. Residual tail on macOS comes from the OS scheduler + `chrono::steady_clock` (~40ns/syscall), not the engine; Linux with `isolcpus` + `HUGETLB` + `rdtsc` timing (M5) should close the remaining gap toward the sub-μs max target.

## Milestones
- **M1 (Week 1):** Order book (price-time priority) + matching + unit tests ✅ **complete**
- **M2 (Week 3):** Intrusive doubly-linked list per level + `ObjectPool<OrderNode>` + lock-free SPSC ring (producer → matcher) ✅ **complete**
- **M3 (Week 5):** MPSC Disruptor pattern, multi-symbol sharding
- **M3 (Week 6):** FIX gateway + market data feed handler
- **M4 (Week 8):** FIX gateway + market data feed handler
- **M5 (Week 12):** Backtester + strategy framework + replay tick data
- **M6 (Week 16):** io_uring/DPDK + sub-microsecond tail latency (Linux isolcpus + HUGETLB)

## Key References
- "Trading and Exchanges" (Harris)
- LMAX Disruptor paper
- Jane Street / HRT engineering blogs
