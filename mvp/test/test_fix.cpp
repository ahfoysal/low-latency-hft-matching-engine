// M3 integration test:
//   1) Unit-test the FIX parser / builder round-trip.
//   2) Stand up the gateway bound to an ephemeral port, open a loopback
//      UDP socket for the market-data feed, then drive 1000 NewOrderSingle
//      messages through a real TCP FIX session and assert we got ExecReports
//      and L1 datagrams back.
//
// Runs on a single process with no external dependencies.

#include "fix/parser.h"
#include "fix/gateway.h"
#include "md/feed.h"
#include "engine.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>

using namespace hft;
using namespace std::chrono_literals;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

static int g_pass = 0;
static void ok(const char* name) {
    ++g_pass;
    std::printf("  ok   %s\n", name);
}

// ---------- Parser unit tests -----------------------------------------------

static void test_parser_roundtrip() {
    fix::Builder b;
    b.begin('D', "CLIENT", "ENGINE", 42);
    b.add_str(fix::ClOrdID, "ORD1");
    b.add_str(fix::Symbol, "AAPL");
    b.add_char(fix::SideTag, '1');
    b.add_double(fix::OrderQty, 100, 0);
    b.add_double(fix::PriceTag, 150.25, 2);
    b.add_char(fix::OrdType, '2');
    auto bytes = b.finalize();

    std::string_view frame;
    std::size_t consumed = 0;
    auto st = fix::split_frame(bytes, &frame, &consumed);
    CHECK(st == fix::FrameStatus::Ok);
    CHECK(consumed == bytes.size());

    fix::Message m;
    CHECK(fix::parse_frame(frame, &m));
    CHECK(m.msg_type() == 'D');
    CHECK(m.get(fix::SenderCompID) == "CLIENT");
    CHECK(m.get(fix::TargetCompID) == "ENGINE");
    CHECK(m.get(fix::ClOrdID) == "ORD1");
    CHECK(m.get(fix::Symbol) == "AAPL");
    CHECK(m.get_int(fix::OrderQty) == 100);
    CHECK(m.get_double(fix::PriceTag) == 150.25);
    ok("parser roundtrip");
}

static void test_parser_incomplete() {
    fix::Builder b;
    b.begin('0', "C", "E", 1);
    auto bytes = b.finalize();
    std::string partial(bytes.substr(0, bytes.size() / 2));

    std::string_view frame;
    std::size_t consumed = 0;
    auto st = fix::split_frame(partial, &frame, &consumed);
    CHECK(st == fix::FrameStatus::Incomplete);
    CHECK(consumed == 0);
    ok("parser incomplete");
}

static void test_parser_checksum_detects_corruption() {
    fix::Builder b;
    b.begin('D', "C", "E", 1);
    b.add_str(fix::ClOrdID, "X");
    std::string bytes(b.finalize());
    // Flip a byte in the body.
    bytes[20] ^= 0x20;
    std::string_view frame;
    std::size_t consumed = 0;
    auto st = fix::split_frame(bytes, &frame, &consumed);
    CHECK(st == fix::FrameStatus::Malformed);
    ok("parser detects corruption via checksum");
}

// ---------- Integration test ------------------------------------------------

// Open a UDP listen socket on ephemeral port.
static int open_udp_listen(uint16_t* out_port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

// Connect a loopback TCP client to the gateway.
static int connect_tcp(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 50; ++i) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return fd;
        std::this_thread::sleep_for(20ms);
    }
    std::fprintf(stderr, "connect failed\n");
    std::exit(1);
}

// Blocking recv of a single complete FIX frame from an fd.
// Accumulates into an external buffer so leftover bytes survive.
static bool recv_one_frame(int fd, std::string& buf, std::string* out_frame,
                           int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    char chunk[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        std::string_view frame;
        std::size_t consumed = 0;
        auto st = fix::split_frame(buf, &frame, &consumed);
        if (st == fix::FrameStatus::Ok) {
            out_frame->assign(frame.data(), frame.size());
            buf.erase(0, consumed);
            return true;
        }
        if (st == fix::FrameStatus::Malformed) return false;

        pollfd pfd{fd, POLLIN, 0};
        int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) break;
        int pr = ::poll(&pfd, 1, left);
        if (pr <= 0) break;
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
    return false;
}

static void test_end_to_end_1000_orders() {
    // 1) Stand up UDP MD listener.
    uint16_t md_port = 0;
    int md_fd = open_udp_listen(&md_port);
    int flags = ::fcntl(md_fd, F_GETFL, 0);
    ::fcntl(md_fd, F_SETFL, flags | O_NONBLOCK);

    // 2) Feed publishes to that UDP port on loopback.
    md::Feed feed(md::FeedConfig{"127.0.0.1", md_port, false, 1});
    CHECK(feed.open());

    // 3) Engine + Gateway on ephemeral TCP port.
    Engine engine;
    fix::GatewayConfig gcfg{"127.0.0.1", 0, "ENGINE"};
    fix::Gateway gw(gcfg, engine, &feed);
    CHECK(gw.start());
    uint16_t gw_port = gw.bound_port();

    // 4) Client: connect, logon, fire orders.
    int fd = connect_tcp(gw_port);
    fix::Builder cb;
    std::string rx;

    // Logon.
    cb.begin('A', "CLIENT", "ENGINE", 1);
    cb.add_int(fix::EncryptMethod, 0);
    cb.add_int(fix::HeartBtInt, 30);
    auto logon_bytes = cb.finalize();
    CHECK(::send(fd, logon_bytes.data(), logon_bytes.size(), 0) ==
          static_cast<ssize_t>(logon_bytes.size()));

    {
        std::string frame;
        CHECK(recv_one_frame(fd, rx, &frame));
        fix::Message m;
        CHECK(fix::parse_frame(frame, &m));
        CHECK(m.msg_type() == 'A');
    }

    constexpr int kOrders = 1000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> px_dist(9990, 10010);
    std::uniform_int_distribution<int> side_dist(0, 1);

    int exec_count = 0;
    int seq = 2;
    std::atomic<int> udp_count{0};

    // UDP drainer thread (non-blocking recv loop).
    std::atomic<bool> drain_run{true};
    std::thread drainer([&] {
        char b[1024];
        while (drain_run.load()) {
            ssize_t n = ::recv(md_fd, b, sizeof(b), 0);
            if (n > 0) {
                udp_count.fetch_add(1);
            } else {
                std::this_thread::sleep_for(500us);
            }
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOrders; ++i) {
        char cid[32];
        std::snprintf(cid, sizeof(cid), "ORD%d", i);
        double px = static_cast<double>(px_dist(rng)) / 100.0;
        char side = side_dist(rng) ? '1' : '2';
        cb.begin('D', "CLIENT", "ENGINE", seq++);
        cb.add_str(fix::ClOrdID, cid);
        cb.add_str(fix::Symbol, "AAPL");
        cb.add_char(fix::SideTag, side);
        cb.add_double(fix::OrderQty, 10, 0);
        cb.add_double(fix::PriceTag, px, 2);
        cb.add_char(fix::OrdType, '2');
        auto bytes = cb.finalize();
        const char* p = bytes.data();
        std::size_t left = bytes.size();
        while (left) {
            ssize_t n = ::send(fd, p, left, 0);
            CHECK(n > 0);
            p += n; left -= static_cast<std::size_t>(n);
        }
    }

    // Drain exec reports. Every order yields at least one ExecReport (NEW or
    // PARTIAL_FILL/FILL), possibly more. We need >= kOrders.
    while (exec_count < kOrders) {
        std::string frame;
        if (!recv_one_frame(fd, rx, &frame, 3000)) break;
        fix::Message m;
        CHECK(fix::parse_frame(frame, &m));
        if (m.msg_type() == '8') ++exec_count;
    }
    auto t1 = std::chrono::steady_clock::now();

    // Market-data request at the end.
    cb.begin('V', "CLIENT", "ENGINE", seq++);
    cb.add_str(fix::MDReqID, "MD1");
    cb.add_char(fix::SubscriptionRequestType, '0');
    cb.add_int(fix::MarketDepth, 1);
    cb.add_int(fix::NoRelatedSym, 1);
    cb.add_str(fix::Symbol, "AAPL");
    auto md_bytes = cb.finalize();
    CHECK(::send(fd, md_bytes.data(), md_bytes.size(), 0) > 0);

    bool got_w = false;
    for (int i = 0; i < 5 && !got_w; ++i) {
        std::string frame;
        if (!recv_one_frame(fd, rx, &frame, 1000)) break;
        fix::Message m;
        CHECK(fix::parse_frame(frame, &m));
        if (m.msg_type() == 'W') got_w = true;
    }

    // Give the UDP drainer a moment to catch up.
    std::this_thread::sleep_for(200ms);
    drain_run.store(false);
    drainer.join();

    // Logout.
    cb.begin('5', "CLIENT", "ENGINE", seq++);
    auto lo = cb.finalize();
    ::send(fd, lo.data(), lo.size(), 0);
    ::close(fd);

    gw.stop();
    feed.close();
    ::close(md_fd);

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          t1 - t0).count();
    std::printf("  info %d orders -> %d exec reports in %lld ms "
                "(%d UDP L1 datagrams, W snapshot=%s)\n",
                kOrders, exec_count,
                static_cast<long long>(elapsed_ms),
                udp_count.load(), got_w ? "yes" : "no");

    CHECK(exec_count >= kOrders);
    CHECK(gw.orders_received() == static_cast<uint64_t>(kOrders));
    CHECK(gw.exec_reports_sent() >= static_cast<uint64_t>(kOrders));
    // UDP is lossy by protocol, but on loopback with small volume we expect
    // essentially all of them. Require at least half to guard against bugs
    // while tolerating kernel-buffer edge cases on loaded CI hosts.
    CHECK(udp_count.load() >= kOrders / 2);
    CHECK(got_w);
    ok("1000-order end-to-end FIX + UDP L1 feed");
}

int main() {
    std::printf("M3 FIX gateway + MD feed tests\n");
    test_parser_roundtrip();
    test_parser_incomplete();
    test_parser_checksum_detects_corruption();
    test_end_to_end_1000_orders();
    std::printf("all %d tests passed\n", g_pass);
    return 0;
}
