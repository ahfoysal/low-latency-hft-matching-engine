#pragma once

#include "pool.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace hft {

using Price     = int64_t;   // price in ticks (integer)
using Quantity  = int64_t;
using OrderId   = uint64_t;
using Timestamp = uint64_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// Intrusive doubly-linked list node. Each price level owns a list of these,
// allocated from an ObjectPool<OrderNode> so `place_limit` does zero heap
// work on the hot path (M1 used std::deque<Order>, which page-fault-spiked
// the tail latency into the ~2ms range).
struct OrderNode {
    OrderId    id;
    Side       side;
    Price      price;
    Quantity   qty;
    Timestamp  ts;
    OrderNode* prev;   // intrusive links
    OrderNode* next;
};

struct Trade {
    OrderId  taker_id;
    OrderId  maker_id;
    Price    price;
    Quantity qty;
};

// A single price level: intrusive FIFO queue of OrderNode*. Head is the
// oldest (matches first), tail is the newest (push_back on arrival).
struct Level {
    OrderNode* head = nullptr;
    OrderNode* tail = nullptr;
    bool empty() const noexcept { return head == nullptr; }
};

// Price-time priority limit order book.
// Bids: descending by price (best = highest). Asks: ascending (best = lowest).
// Within a price level, FIFO via intrusive doubly-linked list.
class OrderBook {
public:
    // Default pool capacity: 2M nodes. Roughly 100 MB on a 64B node — sized to
    // comfortably hold the 1M-order bench plus a safety margin. In production
    // this would be configurable and measured against expected book depth.
    static constexpr std::size_t kDefaultPoolCapacity = 2'000'000;

    explicit OrderBook(std::size_t pool_capacity = kDefaultPoolCapacity);
    ~OrderBook();

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

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
    // Bids: greater<Price> so begin() = best (highest) bid.
    std::map<Price, Level, std::greater<Price>> bids_;
    // Asks: less<Price> so begin() = best (lowest) ask.
    std::map<Price, Level, std::less<Price>>    asks_;

    // Direct pointer to the live node — cancel is O(1) now instead of the
    // level-scan the M1 code did. (The intrusive links give us unlink-in-
    // place for free, so this was the natural upgrade.)
    std::unordered_map<OrderId, OrderNode*> index_;

    ObjectPool<OrderNode> pool_;
    OrderId               next_id_ = 1;
    Timestamp             next_ts_ = 1;

    // Level helpers (inline-friendly; kept in the .cpp to avoid header bloat).
    void level_push_back(Level& lvl, OrderNode* n) noexcept;
    void level_unlink(Level& lvl, OrderNode* n) noexcept;

    template <typename BookSide>
    void match_against(BookSide& opp, OrderId taker_id, Price limit,
                       Quantity& qty, std::vector<Trade>& trades);
};

} // namespace hft
