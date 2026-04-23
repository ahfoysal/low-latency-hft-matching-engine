// Minimal hand-rolled test harness (no gtest dep for MVP).
#include "engine.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace hft;

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        return false;                                                        \
    }                                                                        \
} while (0)

static bool test_simple_buy_rests() {
    Engine e;
    auto id = e.place_limit(Side::Buy, 100, 10);
    CHECK(id == 1);
    CHECK(e.last_trades().empty());
    CHECK(e.book().best_bid() == 100);
    CHECK(e.book().best_ask() == std::numeric_limits<Price>::max());
    CHECK(e.book().qty_at(Side::Buy, 100) == 10);
    return true;
}

static bool test_simple_sell_rests() {
    Engine e;
    e.place_limit(Side::Sell, 101, 7);
    CHECK(e.book().best_ask() == 101);
    CHECK(e.book().qty_at(Side::Sell, 101) == 7);
    CHECK(e.last_trades().empty());
    return true;
}

static bool test_partial_fill() {
    Engine e;
    e.place_limit(Side::Sell, 100, 5);           // resting ask 5 @ 100
    e.place_limit(Side::Buy,  100, 8);           // buy 8 crosses 5
    const auto& t = e.last_trades();
    CHECK(t.size() == 1);
    CHECK(t[0].qty == 5);
    CHECK(t[0].price == 100);
    CHECK(e.book().best_bid() == 100);           // 3 remainder rests as bid
    CHECK(e.book().qty_at(Side::Buy, 100) == 3);
    CHECK(e.book().best_ask() == std::numeric_limits<Price>::max());
    return true;
}

static bool test_full_cross_multiple_levels() {
    Engine e;
    e.place_limit(Side::Sell, 100, 3);
    e.place_limit(Side::Sell, 101, 4);
    e.place_limit(Side::Sell, 102, 5);           // 3 levels on ask side
    e.place_limit(Side::Buy,  101, 10);          // should eat 3@100 + 4@101 = 7, rest 3 @101
    const auto& t = e.last_trades();
    CHECK(t.size() == 2);
    CHECK(t[0].price == 100 && t[0].qty == 3);
    CHECK(t[1].price == 101 && t[1].qty == 4);
    CHECK(e.book().best_bid() == 101);
    CHECK(e.book().qty_at(Side::Buy,  101) == 3);
    CHECK(e.book().best_ask() == 102);
    CHECK(e.book().qty_at(Side::Sell, 102) == 5);
    return true;
}

static bool test_cancel() {
    Engine e;
    auto a = e.place_limit(Side::Buy, 100, 10);
    auto b = e.place_limit(Side::Buy, 100, 20);  // same level, FIFO after a
    CHECK(e.book().qty_at(Side::Buy, 100) == 30);
    CHECK(e.cancel(a));
    CHECK(!e.cancel(a));                          // already gone
    CHECK(e.book().qty_at(Side::Buy, 100) == 20);
    // Cancel last -> level removed.
    CHECK(e.cancel(b));
    CHECK(e.book().best_bid() == 0);
    CHECK(e.book().size() == 0);
    return true;
}

static bool test_price_time_priority_fifo() {
    // Two resting sells at same price; earlier one fills first.
    Engine e;
    auto s1 = e.place_limit(Side::Sell, 100, 4);
    auto s2 = e.place_limit(Side::Sell, 100, 4);
    (void)s2;
    e.place_limit(Side::Buy, 100, 4);
    const auto& t = e.last_trades();
    CHECK(t.size() == 1);
    CHECK(t[0].maker_id == s1);
    CHECK(t[0].qty == 4);
    CHECK(e.book().qty_at(Side::Sell, 100) == 4); // s2 still resting
    return true;
}

int main() {
    struct T { const char* name; bool (*fn)(); };
    T tests[] = {
        {"simple_buy_rests",           test_simple_buy_rests},
        {"simple_sell_rests",          test_simple_sell_rests},
        {"partial_fill",               test_partial_fill},
        {"full_cross_multiple_levels", test_full_cross_multiple_levels},
        {"cancel",                     test_cancel},
        {"price_time_priority_fifo",   test_price_time_priority_fifo},
    };
    int passed = 0, total = 0;
    for (auto& t : tests) {
        ++total;
        bool ok = t.fn();
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", t.name);
        if (ok) ++passed;
    }
    std::printf("\n%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
