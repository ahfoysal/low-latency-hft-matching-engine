#include "order_book.h"

#include <algorithm>
#include <limits>

namespace hft {

OrderBook::OrderBook() = default;

Price OrderBook::best_bid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::best_ask() const {
    return asks_.empty() ? std::numeric_limits<Price>::max()
                         : asks_.begin()->first;
}

Quantity OrderBook::qty_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it == bids_.end()) return 0;
        Quantity q = 0;
        for (auto const& o : it->second) q += o.qty;
        return q;
    } else {
        auto it = asks_.find(price);
        if (it == asks_.end()) return 0;
        Quantity q = 0;
        for (auto const& o : it->second) q += o.qty;
        return q;
    }
}

template <typename BookSide>
void OrderBook::match_against(BookSide& opp, Side /*taker_side*/,
                              OrderId taker_id, Price limit,
                              Quantity& qty, std::vector<Trade>& trades) {
    while (qty > 0 && !opp.empty()) {
        auto level_it = opp.begin();
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

        auto& dq = level_it->second;
        while (qty > 0 && !dq.empty()) {
            Order& maker = dq.front();
            Quantity fill = std::min(qty, maker.qty);
            trades.push_back(Trade{taker_id, maker.id, level_px, fill});
            qty       -= fill;
            maker.qty -= fill;
            if (maker.qty == 0) {
                index_.erase(maker.id);
                dq.pop_front();
            }
        }
        if (dq.empty()) opp.erase(level_it);
    }
}

OrderId OrderBook::place_limit(Side side, Price price, Quantity qty,
                               std::vector<Trade>& trades) {
    OrderId id = next_id_++;
    Quantity remaining = qty;

    if (side == Side::Buy) {
        match_against(asks_, side, id, price, remaining, trades);
        if (remaining > 0) {
            auto& lvl = bids_[price];
            lvl.push_back(Order{id, side, price, remaining, next_ts_++});
            index_[id] = Locator{side, price};
        }
    } else {
        match_against(bids_, side, id, price, remaining, trades);
        if (remaining > 0) {
            auto& lvl = asks_[price];
            lvl.push_back(Order{id, side, price, remaining, next_ts_++});
            index_[id] = Locator{side, price};
        }
    }
    return id;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Side  s  = it->second.side;
    Price px = it->second.price;

    auto erase_from = [&](auto& book) -> bool {
        auto lvl = book.find(px);
        if (lvl == book.end()) return false;
        auto& dq = lvl->second;
        for (auto dit = dq.begin(); dit != dq.end(); ++dit) {
            if (dit->id == id) {
                dq.erase(dit);
                if (dq.empty()) book.erase(lvl);
                return true;
            }
        }
        return false;
    };

    bool ok = (s == Side::Buy) ? erase_from(bids_) : erase_from(asks_);
    if (ok) index_.erase(it);
    return ok;
}

} // namespace hft
