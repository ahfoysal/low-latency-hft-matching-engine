#pragma once

// Moving-average crossover: maintains a fast and a slow SMA over trade
// prints. When fast crosses above slow, go long (buy up to target_position
// with a marketable buy). When fast crosses below slow, go short.
// Position is bounded to +/- target_position.

#include "backtest/strategy.h"

#include <deque>

namespace hft::backtest::strategies {

struct MACrossConfig {
    size_t   fast_window     = 20;
    size_t   slow_window     = 100;
    Quantity target_position = 50;
    Price    slippage_ticks  = 1;   // cross the spread by this many ticks
};

class MACross : public Strategy {
public:
    explicit MACross(MACrossConfig cfg = {})
        : Strategy("ma_cross"), cfg_(cfg) {}

    void on_tick(StrategyContext& ctx, const Tick& t) override;

private:
    MACrossConfig cfg_;

    std::deque<Price> fast_win_;
    std::deque<Price> slow_win_;
    int64_t fast_sum_ = 0;
    int64_t slow_sum_ = 0;
    int     last_sign_ = 0;  // -1, 0, +1

    void push_price(Price p);
    Price fast_avg() const { return fast_win_.empty() ? 0 : fast_sum_ / static_cast<int64_t>(fast_win_.size()); }
    Price slow_avg() const { return slow_win_.empty() ? 0 : slow_sum_ / static_cast<int64_t>(slow_win_.size()); }
};

} // namespace hft::backtest::strategies
