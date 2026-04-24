#include "portfolio.h"

#include <cstdlib>

namespace hft::backtest {

void Portfolio::on_fill(Side side, Price price, Quantity qty) {
    // Normalize to signed delta: buys add, sells subtract.
    const int64_t signed_qty = (side == Side::Buy) ? qty : -qty;
    gross_volume_ += qty;

    if (position_ == 0) {
        // Opening from flat.
        position_ = signed_qty;
        avg_cost_ = price;
        return;
    }

    const bool same_direction =
        (position_ > 0 && signed_qty > 0) || (position_ < 0 && signed_qty < 0);

    if (same_direction) {
        // Weighted-average new cost.
        const int64_t new_pos = position_ + signed_qty;
        // Use absolute magnitudes for the weighted average so signs don't bite.
        const int64_t abs_old = std::abs(position_);
        const int64_t abs_add = std::abs(signed_qty);
        // (old_cost*old_qty + fill_cost*fill_qty) / (old_qty + fill_qty)
        avg_cost_ = (avg_cost_ * abs_old + price * abs_add) / (abs_old + abs_add);
        position_ = new_pos;
        return;
    }

    // Opposite direction: realize on the closed portion.
    const int64_t abs_pos  = std::abs(position_);
    const int64_t abs_fill = std::abs(signed_qty);
    const int64_t closed   = std::min(abs_pos, abs_fill);

    // PnL per unit closed: long  → (price - avg_cost),
    //                       short → (avg_cost - price).
    const int64_t pnl_per_unit = (position_ > 0) ? (price - avg_cost_)
                                                 : (avg_cost_ - price);
    realized_pnl_ += pnl_per_unit * closed;

    if (abs_fill < abs_pos) {
        // Partial close, same sign, avg cost unchanged.
        position_ += signed_qty;
    } else if (abs_fill == abs_pos) {
        // Fully flat.
        position_ = 0;
        avg_cost_ = 0;
    } else {
        // Flipped past zero. Residual opens a new position at fill price.
        position_ = signed_qty + position_; // keeps residual sign of signed_qty
        avg_cost_ = price;
    }
}

int64_t Portfolio::unrealized_pnl(Price mark) const {
    if (position_ == 0) return 0;
    // long: (mark - avg)*pos ; short: (avg - mark)*|pos|
    if (position_ > 0) return (mark - avg_cost_) * position_;
    return (avg_cost_ - mark) * (-position_);
}

} // namespace hft::backtest
