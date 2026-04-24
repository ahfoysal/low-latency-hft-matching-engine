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
- **M3:** hand-rolled FIX 4.4 TCP gateway + UDP L1 market-data feed. Zero external FIX libs — parser is ~180 LOC, gateway ~300. See "M3 Status" below.

### Build

```bash
cd mvp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/test_book          # order book unit tests
./build/test_fix           # FIX gateway + UDP feed integration test
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

## M3 Status — shipped

Network perimeter: a real TCP FIX 4.4 gateway accepts sessions, parses SOH-delimited tag=value wire frames, dispatches into the engine, and emits ExecutionReports back to the originating session. Every book change is also fanned out as a top-of-book (L1) UDP datagram on a separate market-data channel (unicast by default, multicast-capable via a flag in `FeedConfig`).

Scope of the subset implemented:

| Msg | Name                           | Direction          |
|-----|--------------------------------|--------------------|
| A   | Logon                          | client → gateway, ack returned |
| 5   | Logout                         | both ways           |
| 0   | Heartbeat                      | client → gateway    |
| D   | NewOrderSingle                 | client → gateway    |
| F   | OrderCancelRequest             | client → gateway    |
| 8   | ExecutionReport (NEW/PARTIAL/FILL/CANCELED/REJECTED) | gateway → client |
| V   | MarketDataRequest              | client → gateway    |
| W   | MarketDataSnapshotFullRefresh  | gateway → client    |

Layout:

- [`mvp/src/fix/parser.{h,cpp}`](./mvp/src/fix/) — streaming frame splitter + tag/value parser + builder. Checksum (tag 10) is validated on ingress and generated on egress. Zero dependencies beyond libc++.
- [`mvp/src/fix/gateway.{h,cpp}`](./mvp/src/fix/) — POSIX TCP acceptor, one worker thread per session, `TCP_NODELAY`, coarse mutex around the engine (fine for M3; M4 will route through the SPSC ring into a pinned matcher thread).
- [`mvp/src/md/feed.{h,cpp}`](./mvp/src/md/) — UDP publisher. Emits `L1|<seq>|<symbol>|<bid_px>|<bid_sz>|<ask_px>|<ask_sz>\n`. Sequence numbers let consumers detect drops.
- [`mvp/test/test_fix.cpp`](./mvp/test/test_fix.cpp) — parser unit tests + a full end-to-end test that stands up the gateway, opens a UDP listener, logs on, fires 1000 `NewOrderSingle` messages, validates ≥1000 `ExecutionReport`s and L1 datagrams, and checks a `MarketDataSnapshotFullRefresh`.

### Demo

```bash
cd mvp
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/test_fix
```

Sample run (Apple M-series, macOS, loopback):

```
M3 FIX gateway + MD feed tests
  ok   parser roundtrip
  ok   parser incomplete
  ok   parser detects corruption via checksum
  info 1000 orders -> 1000 exec reports in 15 ms (1000 UDP L1 datagrams, W snapshot=yes)
  ok   1000-order end-to-end FIX + UDP L1 feed
all 4 tests passed
```

15 ms for 1000 orders round-tripped through TCP + FIX encode/decode + engine + UDP fan-out = ~67k ord/s single-session on loopback. This is not the engine's ceiling (M2 bench is 8M ord/s in-process) — it's the gateway / socket layer, which M4 will optimize (batched `send`, pinned threads, eventually io_uring).

## M4 Status — shipped

Backtester + strategy framework. A replay engine reads timestamped L2-ish
tick events (Add/Cancel/Trade) from CSV (or a compact binary dump), feeds
them into the same `Engine` used in production, and fans out per-tick
callbacks to a list of `Strategy` subclasses. Each strategy has its own
`Portfolio` (weighted-avg-cost position + realized/unrealized PnL). The
replayer routes engine-side `Trade`s back to the originating strategy via
an `OrderId → Strategy*` table, so fills against exogenous makers do not
contaminate strategy PnL.

Layout:

- [`mvp/src/backtest/replay.{h,cpp}`](./mvp/src/backtest/) — Tick + CSV/binary IO + Replay loop (Max / Realtime speeds).
- [`mvp/src/backtest/strategy.{h,cpp}`](./mvp/src/backtest/) — `Strategy` base + `StrategyContext` (place/cancel routed to owner).
- [`mvp/src/backtest/portfolio.{h,cpp}`](./mvp/src/backtest/) — WAC PnL accounting, including sign-flip closures.
- [`mvp/strategies/mm.{h,cpp}`](./mvp/strategies/) — Symmetric market maker with inventory skew.
- [`mvp/strategies/ma_cross.{h,cpp}`](./mvp/strategies/) — Fast/slow SMA crossover, flips to ±target_position.
- [`mvp/bench/gen_ticks.cpp`](./mvp/bench/gen_ticks.cpp) — Synthetic generator: ~70% Adds, ~20% trade prints, ~10% cancels, mean-reverting mid.
- [`mvp/bench/backtest_main.cpp`](./mvp/bench/backtest_main.cpp) — Loads a tick file, runs replay, prints per-strategy stats.
- [`mvp/test/test_backtest.cpp`](./mvp/test/test_backtest.cpp) — 22 unit tests: portfolio math, CSV roundtrip, replay routing.

### Demo

```bash
cd mvp
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/test_backtest
./build/gen_ticks /tmp/ticks.csv 100000 42
./build/backtest_main /tmp/ticks.csv
```

Sample run on 100k synthetic ticks (Apple M-series, macOS):

```
--- replay stats ---
  ticks=100400 adds=70445 cancels=9821 trades=20134 engine_trades=30842
  wall=146ms  (0.69M ticks/s)

--- per-strategy stats (mark=9999) ---
  market_maker   | submitted=3927  filled=20    canceled=3912 | pos=-53 avg=9999 | realized=+15    total=+15
  ma_cross       | submitted=776   filled=3709  canceled=0    | pos=-20 avg=9999 | realized=-20929 total=-20929
```

The MM holds small inventory near zero and scratches a tiny positive PnL —
exactly the shape you want for a symmetric quoter on a mean-reverting
instrument. The MA crossover bleeds on this series because the synthetic
mid is mean-reverting with no trend (crossover strategies lose on noise by
design); flipping the drift sign in `gen_ticks.cpp` makes it profitable,
which is the expected controlled-experiment behavior.

## Milestones
- **M1 (Week 1):** Order book (price-time priority) + matching + unit tests — **complete**
- **M2 (Week 3):** Intrusive doubly-linked list per level + `ObjectPool<OrderNode>` + lock-free SPSC ring (producer → matcher) — **complete**
- **M3 (Week 6):** FIX 4.4 gateway + UDP market-data feed handler — **complete**
- **M4 (Week 8):** Backtester + strategy framework + replay tick data — **complete**
- **M5 (Week 12):** MPSC Disruptor pattern, multi-symbol sharding, engine on pinned thread behind the SPSC ring
- **M6 (Week 16):** io_uring/DPDK + sub-microsecond tail latency (Linux isolcpus + HUGETLB)

## Key References
- "Trading and Exchanges" (Harris)
- LMAX Disruptor paper
- Jane Street / HRT engineering blogs
