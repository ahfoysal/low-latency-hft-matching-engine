#include "mm.h"

#include "backtest/replay.h"   // Tick definition

#include <cstdlib>

namespace hft::backtest::strategies {

void MarketMaker::cancel_quotes(StrategyContext& ctx) {
    if (bid_id_) { ctx.cancel(bid_id_); bid_id_ = 0; }
    if (ask_id_) { ctx.cancel(ask_id_); ask_id_ = 0; }
}

void MarketMaker::place_quotes(StrategyContext& ctx, Price mid) {
    const Quantity pos = portfolio().position();

    // Inventory skew: if long heavy, only quote the sell side (to reduce);
    // if short heavy, only quote the buy side.
    const bool quote_bid = (pos <= cfg_.inventory_limit);
    const bool quote_ask = (pos >= -cfg_.inventory_limit);

    if (quote_bid) {
        bid_id_ = ctx.place_limit(Side::Buy,  mid - cfg_.spread, cfg_.quote_qty);
    }
    if (quote_ask) {
        ask_id_ = ctx.place_limit(Side::Sell, mid + cfg_.spread, cfg_.quote_qty);
    }
    last_quote_mid_ = mid;
}

void MarketMaker::on_tick(StrategyContext& ctx, const Tick& /*t*/) {
    const BookTop& top = ctx.top();
    if (top.bid_px == 0 || top.ask_px == 0) return; // wait for two-sided book
    const Price mid = top.mid;

    ++tick_counter_;
    const bool mid_moved = (last_quote_mid_ == 0) ||
        (std::abs(mid - last_quote_mid_) >= cfg_.requote_threshold);
    const bool interval_hit = (tick_counter_ % cfg_.requote_interval) == 0;

    // Check if quotes got hit (no longer resting) — we need to detect this
    // so we re-quote the filled side. Simplest: if either id is unknown to
    // the book (filled), always treat as needing requote.
    //
    // Since cancel returns true only if the order is still in the book, we
    // can use the "mid moved" + interval heuristic to force periodic
    // refresh. Cheap enough for a backtest.
    if (!mid_moved && !interval_hit) return;

    cancel_quotes(ctx);
    place_quotes(ctx, mid);
}

} // namespace hft::backtest::strategies
