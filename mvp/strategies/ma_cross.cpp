#include "ma_cross.h"

#include "backtest/replay.h"

namespace hft::backtest::strategies {

void MACross::push_price(Price p) {
    fast_win_.push_back(p); fast_sum_ += p;
    slow_win_.push_back(p); slow_sum_ += p;
    if (fast_win_.size() > cfg_.fast_window) {
        fast_sum_ -= fast_win_.front(); fast_win_.pop_front();
    }
    if (slow_win_.size() > cfg_.slow_window) {
        slow_sum_ -= slow_win_.front(); slow_win_.pop_front();
    }
}

void MACross::on_tick(StrategyContext& ctx, const Tick& t) {
    // Only trade prints drive the signal. Book updates feed execution
    // prices but not the MA.
    if (static_cast<EventType>(t.event) == EventType::Trade) {
        push_price(t.price);
    } else if (ctx.top().mid != 0) {
        // Fall back to mid if we're early and have no prints yet.
        push_price(ctx.top().mid);
    } else {
        return;
    }

    if (slow_win_.size() < cfg_.slow_window) return; // warming up

    const Price f = fast_avg();
    const Price s = slow_avg();
    const int sign = (f > s) ? +1 : (f < s) ? -1 : 0;

    if (sign == 0 || sign == last_sign_) return;
    last_sign_ = sign;

    const BookTop& top = ctx.top();
    if (top.bid_px == 0 || top.ask_px == 0) return;

    const Quantity target = cfg_.target_position;
    const Quantity pos    = portfolio().position();
    // Flip to target: qty = target - pos (long target) or -target - pos (short).
    if (sign > 0) {
        const Quantity need = target - pos;
        if (need > 0) {
            // Marketable buy crosses the ask (+ slippage).
            ctx.place_limit(Side::Buy, top.ask_px + cfg_.slippage_ticks, need);
        }
    } else {
        const Quantity need = pos - (-target);
        if (need > 0) {
            ctx.place_limit(Side::Sell, top.bid_px - cfg_.slippage_ticks, need);
        }
    }
}

} // namespace hft::backtest::strategies
