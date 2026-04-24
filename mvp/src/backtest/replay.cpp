#include "replay.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace hft::backtest {

// ---------- file IO ----------

std::vector<Tick> load_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("load_csv: cannot open " + path);

    std::vector<Tick> out;
    out.reserve(1 << 16);
    std::string line;
    // skip header if present
    if (std::getline(in, line)) {
        if (line.find("ts_ns") == std::string::npos) {
            in.clear();
            in.seekg(0);
        }
    }
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Tick t{};
        char ev = 'A', sd = 'B';
        // ts,symbol,event,side,price,qty,ext_id
        // Use sscanf; format is fixed and controlled by our generator.
        unsigned long long ts = 0, ext = 0;
        unsigned sym = 0;
        long long px = 0, qy = 0;
        int n = std::sscanf(line.c_str(),
                            "%llu,%u,%c,%c,%lld,%lld,%llu",
                            &ts, &sym, &ev, &sd, &px, &qy, &ext);
        if (n < 7) continue;
        t.ts_ns  = ts;
        t.symbol = sym;
        t.event  = (ev == 'C') ? static_cast<uint8_t>(EventType::Cancel)
                 : (ev == 'T') ? static_cast<uint8_t>(EventType::Trade)
                               : static_cast<uint8_t>(EventType::Add);
        t.side   = (sd == 'S') ? 1 : 0;
        t.price  = px;
        t.qty    = qy;
        t.ext_id = ext;
        out.push_back(t);
    }
    return out;
}

std::vector<Tick> load_binary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("load_binary: cannot open " + path);
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    in.seekg(0);
    if (sz < 0 || static_cast<size_t>(sz) % sizeof(Tick) != 0) {
        throw std::runtime_error("load_binary: size not multiple of Tick");
    }
    std::vector<Tick> out(static_cast<size_t>(sz) / sizeof(Tick));
    in.read(reinterpret_cast<char*>(out.data()), sz);
    return out;
}

void save_csv(const std::string& path, const std::vector<Tick>& ticks) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("save_csv: cannot open " + path);
    out << "ts_ns,symbol,event,side,price,qty,ext_id\n";
    for (const Tick& t : ticks) {
        char ev = (t.event == static_cast<uint8_t>(EventType::Cancel)) ? 'C'
                : (t.event == static_cast<uint8_t>(EventType::Trade))  ? 'T'
                                                                       : 'A';
        char sd = (t.side == 1) ? 'S' : 'B';
        out << t.ts_ns << ',' << t.symbol << ',' << ev << ',' << sd << ','
            << t.price << ',' << t.qty << ',' << t.ext_id << '\n';
    }
}

void save_binary(const std::string& path, const std::vector<Tick>& ticks) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("save_binary: cannot open " + path);
    out.write(reinterpret_cast<const char*>(ticks.data()),
              static_cast<std::streamsize>(ticks.size() * sizeof(Tick)));
}

// ---------- replay ----------

namespace {

BookTop snapshot_top(const OrderBook& book, Price last_trade_px) {
    BookTop t;
    Price best_bid = book.best_bid();
    Price best_ask = book.best_ask();
    t.bid_px = (best_bid == 0) ? 0 : best_bid;
    t.ask_px = (best_ask == std::numeric_limits<Price>::max()) ? 0 : best_ask;
    t.bid_qty = (t.bid_px != 0) ? book.qty_at(Side::Buy, t.bid_px) : 0;
    t.ask_qty = (t.ask_px != 0) ? book.qty_at(Side::Sell, t.ask_px) : 0;
    if (t.bid_px != 0 && t.ask_px != 0) {
        t.mid = (t.bid_px + t.ask_px) / 2;
    } else if (t.bid_px != 0) {
        t.mid = t.bid_px;
    } else if (t.ask_px != 0) {
        t.mid = t.ask_px;
    } else {
        t.mid = last_trade_px;
    }
    return t;
}

} // namespace

ReplayStats Replay::run(const std::vector<Tick>& ticks) {
    ReplayStats stats{};

    // Routing table: OrderId → owning strategy. The exogenous flow in the
    // tick file is NOT in this table, so fills against exogenous makers only
    // credit our taker (and vice versa) — correct accounting.
    std::unordered_map<OrderId, Strategy*> routing;
    routing.reserve(1 << 14);

    // Map external ids in the tick file to engine ids so Cancel events
    // target the right resting order.
    std::unordered_map<uint64_t, OrderId> ext_to_eng;
    ext_to_eng.reserve(1 << 14);

    std::vector<StrategyContext> ctxs;
    ctxs.reserve(strategies_.size());
    for (Strategy* s : strategies_) {
        ctxs.emplace_back(s, engine_, routing);
    }

    for (size_t i = 0; i < strategies_.size(); ++i) {
        strategies_[i]->on_start(ctxs[i]);
    }

    Price last_trade_px = 0;
    auto  wall_start    = std::chrono::steady_clock::now();
    uint64_t sim_start_ns = ticks.empty() ? 0 : ticks.front().ts_ns;

    for (const Tick& t : ticks) {
        stats.ticks_consumed++;

        if (cfg_.speed == ReplaySpeed::Realtime) {
            auto target_ns = static_cast<uint64_t>(
                (t.ts_ns - sim_start_ns) / cfg_.realtime_factor);
            auto now    = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               now - wall_start).count();
            if (static_cast<uint64_t>(elapsed) < target_ns) {
                std::this_thread::sleep_for(
                    std::chrono::nanoseconds(target_ns - elapsed));
            }
        }

        switch (static_cast<EventType>(t.event)) {
        case EventType::Add: {
            stats.exogenous_adds++;
            OrderId eng_id = engine_.place_limit(
                static_cast<Side>(t.side), t.price, t.qty);
            if (t.ext_id != 0) ext_to_eng[t.ext_id] = eng_id;
            // Exogenous add may cross strategy-owned resting orders — route
            // those fills to the makers.
            for (const Trade& tr : engine_.last_trades()) {
                stats.engine_trades++;
                last_trade_px = tr.price;
                auto maker_it = routing.find(tr.maker_id);
                if (maker_it != routing.end()) {
                    // Maker side is opposite of taker side.
                    Side maker_side = (static_cast<Side>(t.side) == Side::Buy)
                                          ? Side::Sell : Side::Buy;
                    maker_it->second->on_fill(maker_side, tr.price, tr.qty);
                    maker_it->second->orders_filled++;
                    routing.erase(maker_it);
                }
            }
            break;
        }
        case EventType::Cancel: {
            stats.exogenous_cancels++;
            auto it = ext_to_eng.find(t.ext_id);
            if (it != ext_to_eng.end()) {
                engine_.cancel(it->second);
                ext_to_eng.erase(it);
            }
            break;
        }
        case EventType::Trade: {
            stats.trade_prints++;
            last_trade_px = t.price;
            break;
        }
        }

        BookTop top = snapshot_top(engine_.book(), last_trade_px);
        for (size_t i = 0; i < strategies_.size(); ++i) {
            ctxs[i].set_top(top);
            strategies_[i]->on_tick(ctxs[i], t);
            // Strategy orders that immediately filled were already routed
            // inside StrategyContext::place_limit.
            // Count any additional engine trades those placements produced.
            // (engine.last_trades() was consumed there; nothing extra here.)
        }
    }

    for (size_t i = 0; i < strategies_.size(); ++i) {
        strategies_[i]->on_end(ctxs[i]);
    }
    return stats;
}

} // namespace hft::backtest
