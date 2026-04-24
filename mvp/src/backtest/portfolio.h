#pragma once

// Per-strategy portfolio + PnL. Single-symbol for M4 (multi-symbol would
// keep a map<symbol, Position>). Tracks position, average cost (for realized
// PnL), realized PnL, and can compute unrealized PnL against a mark price.
//
// Realized PnL accounting uses weighted-average-cost with sign flips:
//   - Same-direction fill: roll position, update avg cost.
//   - Opposite-direction fill that reduces or closes position: realize PnL
//     on the closed portion at (fill_px - avg_cost) * closed_qty * sign.
//   - Opposite-direction fill that flips position past zero: realize on the
//     closed portion, then open the residual at fill_px as new avg cost.
//
// Prices are in tick-units (int64). PnL is in tick*qty units — callers can
// multiply by tick size to get currency, but for a self-contained backtest
// we just report the raw number. It's directionally identical.

#include "order_book.h"  // Price, Quantity, Side

#include <cstdint>

namespace hft::backtest {

class Portfolio {
public:
    void on_fill(Side side, Price price, Quantity qty);

    // Mark-to-market against `mark` price. Does not mutate realized PnL.
    int64_t unrealized_pnl(Price mark) const;
    int64_t realized_pnl()             const { return realized_pnl_; }
    int64_t total_pnl(Price mark)      const { return realized_pnl_ + unrealized_pnl(mark); }

    Quantity position()   const { return position_; }   // + long, - short
    Price    avg_cost()   const { return avg_cost_; }   // valid iff |position| > 0
    int64_t  gross_vol()  const { return gross_volume_; } // sum of |qty| filled

private:
    Quantity position_     = 0;
    Price    avg_cost_     = 0;
    int64_t  realized_pnl_ = 0;
    int64_t  gross_volume_ = 0;
};

} // namespace hft::backtest
