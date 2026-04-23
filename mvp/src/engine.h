#pragma once

#include "order_book.h"
#include <vector>

namespace hft {

// Thin wrapper around OrderBook presenting the MVP engine API
// (place_limit / cancel / match). Matching happens on place_limit;
// the `match()` call is retained for API shape + future batching.
class Engine {
public:
    Engine() = default;

    OrderId place_limit(Side side, Price price, Quantity qty) {
        trades_scratch_.clear();
        OrderId id = book_.place_limit(side, price, qty, trades_scratch_);
        last_trades_ = trades_scratch_;
        return id;
    }

    bool cancel(OrderId id) { return book_.cancel(id); }

    // No-op in MVP (matching is synchronous on placement). Returns count of
    // trades produced by the last placement.
    size_t match() { return last_trades_.size(); }

    const std::vector<Trade>& last_trades() const { return last_trades_; }

    OrderBook&       book()       { return book_; }
    const OrderBook& book() const { return book_; }

private:
    OrderBook          book_;
    std::vector<Trade> trades_scratch_;
    std::vector<Trade> last_trades_;
};

} // namespace hft
