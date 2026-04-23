# 05 — HFT Trading Engine

**Stack:** C++20 (core) · Rust (gateways/tools) · LMAX Disruptor-style ring buffers · io_uring (later DPDK) · FIX 4.4 + SBE binary protocol · Google Benchmark · perf/flamegraph · Linux isolcpus + HUGEPAGES

## Full Vision
Sub-microsecond matching engine, kernel bypass, colocated deploy, PTP nanosecond clock sync, backtester with L3 tick data, risk engine, strategy DSL.

## MVP (1 weekend)
In-memory limit order book with `place/cancel/match` — target <10μs p99 single-threaded.

## Milestones
- **M1 (Week 1):** Order book (price-time priority) + matching + unit tests
- **M2 (Week 3):** Lock-free SPSC/MPSC queues + Disruptor pattern
- **M3 (Week 6):** FIX gateway + market data feed handler
- **M4 (Week 10):** Backtester + strategy framework + replay tick data
- **M5 (Week 16):** io_uring/DPDK + sub-microsecond tail latency

## Key References
- "Trading and Exchanges" (Harris)
- LMAX Disruptor paper
- Jane Street / HRT engineering blogs
