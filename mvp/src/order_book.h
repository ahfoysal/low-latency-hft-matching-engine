#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>
#include <functional>

namespace hft {

using Price    = int64_t;   // price in ticks (integer)
using Quantity = int64_t;
using OrderId  = uint64_t;
using Timestamp = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;           // remaining quantity
    Timestamp ts;           // sequence / arrival time
};

struct Trade {
    OrderId  taker_id;
    OrderId  maker_id;
    Price    price;
    Quantity qty;
};

// Price-time priority limit order book.
// Bids: descending by price (best = highest). Asks: ascending (best = lowest).
// Within a price level, FIFO via std::deque.
class OrderBook {
public:
    OrderBook();

    // Place a limit order; crosses against the opposite side first. Returns
    // the assigned order id. Any unfilled remainder rests on the book.
    OrderId place_limit(Side side, Price price, Quantity qty,
                        std::vector<Trade>& trades);

    // Cancel an order. Returns true if found & removed.
    bool cancel(OrderId id);

    // Best bid / ask price. Returns 0 (bid) / INT64_MAX (ask) if empty.
    Price best_bid() const;
    Price best_ask() const;

    // Total resting quantity at a given price level (0 if empty). For tests.
    Quantity qty_at(Side side, Price price) const;

    // Number of resting orders currently in the book.
    size_t size() const { return index_.size(); }

private:
    using Level = std::deque<Order>;
    // Bids: greater<Price> so begin() = best (highest) bid.
    std::map<Price, Level, std::greater<Price>> bids_;
    // Asks: less<Price> so begin() = best (lowest) ask.
    std::map<Price, Level, std::less<Price>>    asks_;

    struct Locator {
        Side  side;
        Price price;
        // Pointer to the Order inside its deque. Stable as long as we only
        // pop_front / push_back on the deque (deque guarantees pointer
        // stability for insertions at ends + non-end erasures invalidate
        // only iterators; pointers to non-erased elements remain valid for
        // push_back. We treat ids via scanning the level for cancel for
        // correctness here — small-level assumption keeps MVP simple.)
    };

    std::unordered_map<OrderId, Locator> index_;
    OrderId   next_id_   = 1;
    Timestamp next_ts_   = 1;

    template <typename BookSide>
    void match_against(BookSide& opp, Side taker_side, OrderId taker_id,
                       Price limit, Quantity& qty,
                       std::vector<Trade>& trades);
};

} // namespace hft
