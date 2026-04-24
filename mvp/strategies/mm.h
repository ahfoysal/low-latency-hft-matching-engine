#pragma once

// Simple symmetric market maker. Quotes size `quote_qty` on both sides at
// `spread` ticks away from the current mid. Cancels and re-quotes whenever
// mid moves more than `requote_threshold` ticks or on every N ticks.
// Inventory control: when |position| > inventory_limit, only one-sided
// quotes on the reducing side.

#include "backtest/strategy.h"

namespace hft::backtest::strategies {

struct MarketMakerConfig {
    Quantity quote_qty          = 10;
    Price    spread             = 2;     // half-spread each side
    Price    requote_threshold  = 1;     // re-quote if mid shifts this many ticks
    uint64_t requote_interval   = 50;    // force re-quote every N ticks
    Quantity inventory_limit    = 100;   // soft cap; above this, quote one side
};

class MarketMaker : public Strategy {
public:
    explicit MarketMaker(MarketMakerConfig cfg = {})
        : Strategy("market_maker"), cfg_(cfg) {}

    void on_tick(StrategyContext& ctx, const Tick& t) override;

private:
    MarketMakerConfig cfg_;
    Price             last_quote_mid_ = 0;
    uint64_t          tick_counter_   = 0;
    OrderId           bid_id_         = 0;
    OrderId           ask_id_         = 0;

    void cancel_quotes(StrategyContext& ctx);
    void place_quotes(StrategyContext& ctx, Price mid);
};

} // namespace hft::backtest::strategies
