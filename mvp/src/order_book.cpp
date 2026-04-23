#include "order_book.h"

#include <algorithm>
#include <limits>
#include <new>

namespace hft {

OrderBook::OrderBook(std::size_t pool_capacity)
    : pool_(pool_capacity) {
    // Reserve the index too — one of the remaining tail-latency sources after
    // killing the deque allocations is rehashing in the OrderId -> node map.
    // Sizing it to the pool capacity means we never rehash in steady state.
    index_.reserve(pool_capacity);
}

OrderBook::~OrderBook() {
    // Walk every resting order and release it back to the pool so the pool
    // destructor sees a clean free-list. Not strictly required (the pool
    // owns its slab) but keeps in_use() honest for anyone inspecting state.
    for (auto& [id, node] : index_) {
        node->~OrderNode();
        pool_.release(node);
    }
}

Price OrderBook::best_bid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::best_ask() const {
    return asks_.empty() ? std::numeric_limits<Price>::max()
                         : asks_.begin()->first;
}

Quantity OrderBook::qty_at(Side side, Price price) const {
    auto sum_level = [](const Level& lvl) {
        Quantity q = 0;
        for (const OrderNode* n = lvl.head; n != nullptr; n = n->next) {
            q += n->qty;
        }
        return q;
    };
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return (it == bids_.end()) ? 0 : sum_level(it->second);
    }
    auto it = asks_.find(price);
    return (it == asks_.end()) ? 0 : sum_level(it->second);
}

void OrderBook::level_push_back(Level& lvl, OrderNode* n) noexcept {
    n->prev = lvl.tail;
    n->next = nullptr;
    if (lvl.tail) lvl.tail->next = n;
    else          lvl.head       = n;
    lvl.tail = n;
}

void OrderBook::level_unlink(Level& lvl, OrderNode* n) noexcept {
    if (n->prev) n->prev->next = n->next;
    else         lvl.head      = n->next;
    if (n->next) n->next->prev = n->prev;
    else         lvl.tail      = n->prev;
    n->prev = n->next = nullptr;
}

template <typename BookSide>
void OrderBook::match_against(BookSide& opp, OrderId taker_id, Price limit,
                              Quantity& qty, std::vector<Trade>& trades) {
    while (qty > 0 && !opp.empty()) {
        auto  level_it = opp.begin();
        Price level_px = level_it->first;

        // For a buy taker, opp is asks (ascending): cross if ask <= limit.
        // For a sell taker, opp is bids (descending): cross if bid >= limit.
        bool cross;
        if constexpr (std::is_same_v<typename BookSide::key_compare,
                                     std::less<Price>>) {
            cross = level_px <= limit;        // buy side crossing asks
        } else {
            cross = level_px >= limit;        // sell side crossing bids
        }
        if (!cross) break;

        Level& lvl = level_it->second;
        while (qty > 0 && !lvl.empty()) {
            OrderNode* maker = lvl.head;
            Quantity fill    = std::min(qty, maker->qty);
            trades.push_back(Trade{taker_id, maker->id, level_px, fill});
            qty        -= fill;
            maker->qty -= fill;
            if (maker->qty == 0) {
                index_.erase(maker->id);
                level_unlink(lvl, maker);
                maker->~OrderNode();
                pool_.release(maker);
            }
        }
        if (lvl.empty()) opp.erase(level_it);
    }
}

OrderId OrderBook::place_limit(Side side, Price price, Quantity qty,
                               std::vector<Trade>& trades) {
    OrderId  id        = next_id_++;
    Quantity remaining = qty;

    if (side == Side::Buy) {
        match_against(asks_, id, price, remaining, trades);
        if (remaining > 0) {
            OrderNode* n = pool_.acquire();
            // Pool exhaustion is a hard stop in M2 — in production we'd fall
            // back to a larger reserve or reject the order upstream.
            if (!n) [[unlikely]] return id;
            ::new (n) OrderNode{id, side, price, remaining, next_ts_++,
                                nullptr, nullptr};
            Level& lvl = bids_[price];
            level_push_back(lvl, n);
            index_[id] = n;
        }
    } else {
        match_against(bids_, id, price, remaining, trades);
        if (remaining > 0) {
            OrderNode* n = pool_.acquire();
            if (!n) [[unlikely]] return id;
            ::new (n) OrderNode{id, side, price, remaining, next_ts_++,
                                nullptr, nullptr};
            Level& lvl = asks_[price];
            level_push_back(lvl, n);
            index_[id] = n;
        }
    }
    return id;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    OrderNode* n     = it->second;
    Price      px    = n->price;
    Side       s     = n->side;

    // O(1) unlink — the intrusive links are the whole reason we refactored.
    if (s == Side::Buy) {
        auto lvl_it = bids_.find(px);
        if (lvl_it == bids_.end()) return false; // should never happen
        level_unlink(lvl_it->second, n);
        if (lvl_it->second.empty()) bids_.erase(lvl_it);
    } else {
        auto lvl_it = asks_.find(px);
        if (lvl_it == asks_.end()) return false;
        level_unlink(lvl_it->second, n);
        if (lvl_it->second.empty()) asks_.erase(lvl_it);
    }

    index_.erase(it);
    n->~OrderNode();
    pool_.release(n);
    return true;
}

} // namespace hft
