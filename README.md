# HFT Trading Engine

**Stack:** C++20 (core) · Rust (gateways/tools) · LMAX Disruptor-style ring buffers · io_uring (later DPDK) · FIX 4.4 + SBE binary protocol · Google Benchmark · perf/flamegraph · Linux isolcpus + HUGEPAGES

## Full Vision
Sub-microsecond matching engine, kernel bypass, colocated deploy, PTP nanosecond clock sync, backtester with L3 tick data, risk engine, strategy DSL.

## MVP (1 weekend)
In-memory limit order book with `place/cancel/match` — target <10μs p99 single-threaded.

## MVP Status — shipped

Single-threaded limit order book with price-time priority, matching on placement, and cancel. Bid/ask sides are `std::map<Price, std::deque<Order>>` (lock-free queues come in M2). Lives under [`mvp/`](./mvp).

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

| metric      | value            |
|-------------|------------------|
| throughput  | 5,599,824 ops/s  |
| latency p50 | 84 ns            |
| latency p90 | 250 ns           |
| latency p99 | 500 ns           |
| latency p99.9 | 1458 ns        |
| latency max | 1.80 ms (alloc / page fault spike) |

p99 well under the 10μs MVP goal. Max-tail spikes are expected from `std::map` node allocation; M2 (arena + intrusive lists + lock-free ring) should flatten the tail.

## Milestones
- **M1 (Week 1):** Order book (price-time priority) + matching + unit tests ✅ **MVP complete**
- **M2 (Week 3):** Lock-free SPSC/MPSC queues + Disruptor pattern
- **M3 (Week 6):** FIX gateway + market data feed handler
- **M4 (Week 10):** Backtester + strategy framework + replay tick data
- **M5 (Week 16):** io_uring/DPDK + sub-microsecond tail latency

## Key References
- "Trading and Exchanges" (Harris)
- LMAX Disruptor paper
- Jane Street / HRT engineering blogs
