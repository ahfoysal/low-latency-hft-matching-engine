#pragma once

// Tick replay: reads a timestamped stream of exchange events (orders + trades)
// and plays them into the engine in order, invoking a list of Strategies
// between ticks so they can observe the book and post their own orders.
//
// The event stream intentionally covers both "exogenous flow" (other
// participants placing/cancelling orders that drive the book) and trade
// prints, which strategies use as a price-series signal. Strategy-generated
// orders are routed through the same Engine so their PnL is mark-to-market
// against the same book their signals are derived from.

#include "engine.h"
#include "strategy.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace hft::backtest {

enum class EventType : uint8_t {
    Add    = 0,   // exogenous order placement (resting or marketable)
    Cancel = 1,   // cancel by external order id (best-effort, ignored if unknown)
    Trade  = 2,   // trade print from the tape (price signal only; no book impact)
};

struct Tick {
    uint64_t ts_ns   = 0;      // nanoseconds since epoch (monotonic in file)
    uint32_t symbol  = 0;      // symbol id (single-symbol backtest keeps this 0)
    uint8_t  event   = 0;      // EventType
    uint8_t  side    = 0;      // 0=buy, 1=sell (ignored for Trade prints by engine)
    int64_t  price   = 0;      // ticks
    int64_t  qty     = 0;
    uint64_t ext_id  = 0;      // external order id (for Cancel linkage)
};

// CSV format:
//   ts_ns,symbol,event,side,price,qty,ext_id
//   where event in {A,C,T}, side in {B,S}
struct CsvFormat {};
// Compact binary format: raw Tick struct dump (little-endian host).
struct BinaryFormat {};

std::vector<Tick> load_csv(const std::string& path);
std::vector<Tick> load_binary(const std::string& path);
void              save_csv(const std::string& path, const std::vector<Tick>& ticks);
void              save_binary(const std::string& path, const std::vector<Tick>& ticks);

enum class ReplaySpeed {
    Max,       // replay as fast as possible (what backtests normally want)
    Realtime,  // sleep to match wall-clock gaps (useful for smoke testing)
};

struct ReplayConfig {
    ReplaySpeed speed = ReplaySpeed::Max;
    // Only honored in Realtime mode; 1.0 = wall-clock, 10.0 = 10x faster.
    double      realtime_factor = 1.0;
};

struct ReplayStats {
    uint64_t ticks_consumed    = 0;
    uint64_t exogenous_adds    = 0;
    uint64_t exogenous_cancels = 0;
    uint64_t trade_prints      = 0;
    uint64_t engine_trades     = 0; // trades produced by the engine (our + external)
};

class Replay {
public:
    Replay(Engine& engine, const ReplayConfig& cfg = {})
        : engine_(engine), cfg_(cfg) {}

    void add_strategy(Strategy* s) { strategies_.push_back(s); }

    // Run the full tick stream. Strategies receive on_tick callbacks with
    // an up-to-date top-of-book snapshot taken AFTER the tick is applied.
    ReplayStats run(const std::vector<Tick>& ticks);

private:
    Engine&                engine_;
    ReplayConfig           cfg_;
    std::vector<Strategy*> strategies_;
};

} // namespace hft::backtest
