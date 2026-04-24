// Backtest driver: loads a tick CSV, constructs the engine + a list of
// strategies, runs Replay at max speed, and prints per-strategy stats.
//
// Usage:
//   ./backtest <ticks.csv>

#include "backtest/replay.h"
#include "backtest/strategy.h"
#include "strategies/mm.h"
#include "strategies/ma_cross.h"

#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

using namespace hft;
using namespace hft::backtest;

static void print_strategy(const Strategy& s, Price mark) {
    const Portfolio& p = s.portfolio();
    std::printf(
        "  %-14s | submitted=%-7llu filled=%-7llu canceled=%-7llu | "
        "pos=%+lld avg=%lld | realized=%+lld unrealized=%+lld total=%+lld | "
        "vol=%lld\n",
        s.name().c_str(),
        (unsigned long long)s.orders_submitted,
        (unsigned long long)s.orders_filled,
        (unsigned long long)s.orders_canceled,
        (long long)p.position(),
        (long long)p.avg_cost(),
        (long long)p.realized_pnl(),
        (long long)p.unrealized_pnl(mark),
        (long long)p.total_pnl(mark),
        (long long)p.gross_vol());
}

int main(int argc, char** argv) {
    std::string path = (argc >= 2) ? argv[1] : "ticks.csv";

    std::printf("loading ticks from %s ...\n", path.c_str());
    std::vector<Tick> ticks = load_csv(path);
    std::printf("loaded %zu ticks\n", ticks.size());

    Engine engine;
    Replay replay(engine);

    strategies::MarketMaker mm(strategies::MarketMakerConfig{
        /*quote_qty*/         5,
        /*spread*/            2,
        /*requote_threshold*/ 1,
        /*requote_interval*/  100,
        /*inventory_limit*/   50,
    });
    strategies::MACross mac(strategies::MACrossConfig{
        /*fast_window*/     20,
        /*slow_window*/     100,
        /*target_position*/ 20,
        /*slippage_ticks*/  1,
    });

    replay.add_strategy(&mm);
    replay.add_strategy(&mac);

    auto t0 = std::chrono::steady_clock::now();
    ReplayStats st = replay.run(ticks);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    Price mark = engine.book().best_bid();
    Price ask  = engine.book().best_ask();
    if (mark && ask && ask != std::numeric_limits<Price>::max()) {
        mark = (mark + ask) / 2;
    } else if (ask != std::numeric_limits<Price>::max() && ask != 0) {
        mark = ask;
    }

    std::printf("\n--- replay stats ---\n");
    std::printf("  ticks=%llu adds=%llu cancels=%llu trades=%llu engine_trades=%llu\n",
                (unsigned long long)st.ticks_consumed,
                (unsigned long long)st.exogenous_adds,
                (unsigned long long)st.exogenous_cancels,
                (unsigned long long)st.trade_prints,
                (unsigned long long)st.engine_trades);
    std::printf("  wall=%lldms  (%.2fM ticks/s)\n",
                (long long)ms,
                ms > 0 ? (double)st.ticks_consumed / (ms * 1000.0) : 0.0);

    std::printf("\n--- per-strategy stats (mark=%lld) ---\n", (long long)mark);
    print_strategy(mm,  mark);
    print_strategy(mac, mark);

    auto fill_rate = [](const Strategy& s) {
        return s.orders_submitted == 0 ? 0.0
            : 100.0 * (double)s.orders_filled / (double)s.orders_submitted;
    };
    std::printf("\n--- fill rates ---\n");
    std::printf("  %-14s %.2f%%\n", mm.name().c_str(),  fill_rate(mm));
    std::printf("  %-14s %.2f%%\n", mac.name().c_str(), fill_rate(mac));
    return 0;
}
