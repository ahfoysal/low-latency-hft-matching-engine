#pragma once

// Strategy framework. A Strategy observes ticks and the current top-of-book,
// and places/cancels orders through a StrategyContext which routes to the
// same Engine the replayer drives. Fills land in the strategy's Portfolio,
// mark-to-market uses mid.
//
// Design note: we keep the strategy-vs-engine identity cleanly separated.
// The replayer maps every Engine-generated Trade back to its originating
// strategy (by tracking which OrderIds we issued on behalf of whom), so
// exogenous fills against exogenous makers do not pollute strategy PnL.

#include "engine.h"
#include "portfolio.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::backtest {

struct Tick; // fwd (defined in replay.h)

struct BookTop {
    Price    bid_px = 0;
    Quantity bid_qty = 0;
    Price    ask_px = 0;
    Quantity ask_qty = 0;
    // Fair mid used for mark-to-market. Defined even if one side is empty
    // (falls back to the other side, or last trade).
    Price    mid   = 0;
};

class Strategy;

// Context passed to strategies on each callback. Tracks which orders were
// issued by this strategy so fills route to its portfolio.
class StrategyContext {
public:
    StrategyContext(Strategy* owner, Engine& eng,
                    std::unordered_map<OrderId, Strategy*>& routing)
        : owner_(owner), engine_(eng), routing_(routing) {}

    // Place a limit order attributed to this strategy. Returns the OrderId.
    OrderId place_limit(Side side, Price price, Quantity qty);
    bool    cancel(OrderId id);

    const BookTop& top() const { return top_; }
    void           set_top(const BookTop& t) { top_ = t; }

    Engine& engine() { return engine_; }

private:
    Strategy*                               owner_;
    Engine&                                 engine_;
    std::unordered_map<OrderId, Strategy*>& routing_;
    BookTop                                 top_;
};

class Strategy {
public:
    explicit Strategy(std::string name) : name_(std::move(name)) {}
    virtual ~Strategy() = default;

    // Called once by the replayer before the first tick.
    virtual void on_start(StrategyContext& /*ctx*/) {}
    // Called after every tick, with the post-tick top-of-book already set
    // on ctx.top().
    virtual void on_tick(StrategyContext& ctx, const Tick& t) = 0;
    // Called once after the last tick; a chance to flush stats.
    virtual void on_end(StrategyContext& /*ctx*/) {}

    // Fill notification from the replayer. side/qty from the strategy's
    // perspective: if we were the taker and our order was a Buy, side=Buy.
    virtual void on_fill(Side side, Price price, Quantity qty) {
        portfolio_.on_fill(side, price, qty);
    }

    const std::string& name() const { return name_; }
    Portfolio&         portfolio()       { return portfolio_; }
    const Portfolio&   portfolio() const { return portfolio_; }

    // Per-strategy counters.
    uint64_t orders_submitted = 0;
    uint64_t orders_filled    = 0; // count of distinct fills (partial or full)
    uint64_t orders_canceled  = 0;

private:
    std::string name_;
    Portfolio   portfolio_;
};

} // namespace hft::backtest
