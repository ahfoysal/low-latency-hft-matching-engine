#include "strategy.h"

namespace hft::backtest {

OrderId StrategyContext::place_limit(Side side, Price price, Quantity qty) {
    // Snapshot next_id BEFORE submission — the engine increments monotonically,
    // so the id we just issued is the one the engine returned. We route any
    // future trade carrying this id to `owner_` so fills land in its portfolio.
    OrderId id = engine_.place_limit(side, price, qty);
    routing_[id] = owner_;
    owner_->orders_submitted++;

    // Strategy-generated marketable orders may fill immediately against the
    // book. Deliver those fills synchronously so on_tick sees an up-to-date
    // position when it next looks at ctx.
    for (const Trade& tr : engine_.last_trades()) {
        // Taker side of every trade from THIS placement is `side`. If the
        // maker was owned by another strategy, route that side too.
        owner_->on_fill(side, tr.price, tr.qty);
        owner_->orders_filled++;

        auto maker_it = routing_.find(tr.maker_id);
        if (maker_it != routing_.end()) {
            Side opposite = (side == Side::Buy) ? Side::Sell : Side::Buy;
            maker_it->second->on_fill(opposite, tr.price, tr.qty);
            maker_it->second->orders_filled++;
        }
    }
    return id;
}

bool StrategyContext::cancel(OrderId id) {
    if (engine_.cancel(id)) {
        owner_->orders_canceled++;
        routing_.erase(id);
        return true;
    }
    return false;
}

} // namespace hft::backtest
