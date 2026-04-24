// Backtest unit tests: portfolio math + a tiny replay sanity check.

#include "backtest/portfolio.h"
#include "backtest/replay.h"
#include "backtest/strategy.h"
#include "engine.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace hft;
using namespace hft::backtest;

static int failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL %s\n", msg); ++failed; } \
    else          std::printf("  ok   %s\n", msg); \
} while (0)

static void test_portfolio_long_only() {
    Portfolio p;
    p.on_fill(Side::Buy, 100, 10);   // long 10 @ 100
    CHECK(p.position() == 10, "long open pos");
    CHECK(p.avg_cost() == 100, "long open avg");
    CHECK(p.realized_pnl() == 0, "no realized yet");
    p.on_fill(Side::Sell, 110, 4);   // close 4 at +10 each = +40
    CHECK(p.position() == 6, "partial close pos");
    CHECK(p.realized_pnl() == 40, "partial realized");
    p.on_fill(Side::Sell, 120, 6);   // close remaining 6 at +20 each = +120
    CHECK(p.position() == 0, "flat");
    CHECK(p.realized_pnl() == 40 + 120, "full realized");
}

static void test_portfolio_short_and_flip() {
    Portfolio p;
    p.on_fill(Side::Sell, 100, 10);  // short 10 @ 100
    CHECK(p.position() == -10, "short open");
    p.on_fill(Side::Buy,  90, 10);   // cover at 90: (100-90)*10 = +100
    CHECK(p.position() == 0, "covered");
    CHECK(p.realized_pnl() == 100, "short pnl");

    Portfolio q;
    q.on_fill(Side::Buy, 100, 5);    // long 5 @ 100
    q.on_fill(Side::Sell, 120, 8);   // close 5 at +20 each (=+100), then short 3 @ 120
    CHECK(q.position() == -3, "flipped short");
    CHECK(q.avg_cost() == 120, "flipped avg");
    CHECK(q.realized_pnl() == 100, "flip realized");
}

static void test_portfolio_unrealized() {
    Portfolio p;
    p.on_fill(Side::Buy, 100, 10);
    CHECK(p.unrealized_pnl(105) == 50, "unrealized long");
    CHECK(p.total_pnl(105)      == 50, "total long");
    p.on_fill(Side::Sell, 110, 10);
    CHECK(p.unrealized_pnl(999) == 0, "flat unrealized");
    CHECK(p.total_pnl(999)      == 100, "total realized-only");
}

namespace {
// Tiny strategy that buys 1 on the first tick, then does nothing — used to
// verify wiring + routing of fills.
class BuyOnce : public Strategy {
public:
    BuyOnce() : Strategy("buy_once") {}
    void on_tick(StrategyContext& ctx, const Tick&) override {
        if (fired_) return;
        const BookTop& t = ctx.top();
        if (t.ask_px == 0) return;
        ctx.place_limit(Side::Buy, t.ask_px, 5);
        fired_ = true;
    }
private:
    bool fired_ = false;
};
} // namespace

static void test_replay_routing() {
    std::vector<Tick> ticks;
    auto add = [&](EventType e, Side s, Price px, Quantity q, uint64_t ext=0) {
        Tick t{};
        t.ts_ns = ticks.empty() ? 1 : ticks.back().ts_ns + 1;
        t.event = static_cast<uint8_t>(e);
        t.side  = static_cast<uint8_t>(s);
        t.price = px; t.qty = q; t.ext_id = ext;
        ticks.push_back(t);
    };
    // Exogenous sell rests at 100, then a dummy trade print so strategy sees
    // a tick and fires its buy order.
    add(EventType::Add,   Side::Sell, 100, 10, 1);
    add(EventType::Add,   Side::Buy,   99, 10, 2);
    add(EventType::Trade, Side::Buy,  100, 1);

    Engine e;
    Replay r(e);
    BuyOnce strat;
    r.add_strategy(&strat);
    ReplayStats st = r.run(ticks);
    (void)st;

    // Strategy should have been filled by crossing the resting 100 ask.
    CHECK(strat.portfolio().position() == 5, "strategy bought 5");
    CHECK(strat.orders_submitted == 1, "one order submitted");
    CHECK(strat.orders_filled >= 1, "at least one fill");
}

static void test_csv_roundtrip() {
    std::vector<Tick> in;
    in.reserve(10);
    for (int i = 0; i < 10; ++i) {
        Tick t{};
        t.ts_ns  = 1000 + i;
        t.symbol = 0;
        t.event  = static_cast<uint8_t>(i % 3);
        t.side   = i % 2;
        t.price  = 10000 + i;
        t.qty    = i + 1;
        t.ext_id = i + 100;
        in.push_back(t);
    }
    const std::string path = "/tmp/hft_test_ticks.csv";
    save_csv(path, in);
    auto out = load_csv(path);
    CHECK(out.size() == in.size(), "csv size roundtrip");
    for (size_t i = 0; i < in.size(); ++i) {
        if (out[i].ts_ns != in[i].ts_ns || out[i].price != in[i].price ||
            out[i].qty != in[i].qty || out[i].event != in[i].event) {
            std::printf("    mismatch at %zu\n", i);
            ++failed;
            return;
        }
    }
    std::printf("  ok   csv field-level roundtrip\n");
}

int main() {
    std::printf("M4 backtester tests\n");
    test_portfolio_long_only();
    test_portfolio_short_and_flip();
    test_portfolio_unrealized();
    test_replay_routing();
    test_csv_roundtrip();
    if (failed) {
        std::printf("%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
