#include "gateway.h"
#include "../md/feed.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace hft::fix {

// FIX ExecType / OrdStatus values we emit.
namespace {
constexpr char EX_NEW           = '0';
constexpr char EX_PARTIAL_FILL  = '1';
constexpr char EX_FILL          = '2';
constexpr char EX_CANCELED      = '4';
constexpr char EX_REJECTED      = '8';

constexpr char OS_NEW           = '0';
constexpr char OS_PARTIAL_FILL  = '1';
constexpr char OS_FILLED        = '2';
constexpr char OS_CANCELED      = '4';
constexpr char OS_REJECTED      = '8';
}  // namespace

Gateway::Gateway(GatewayConfig cfg, Engine& engine, md::Feed* feed)
    : cfg_(std::move(cfg)), engine_(engine), feed_(feed) {}

Gateway::~Gateway() { stop(); }

bool Gateway::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.bind_port);
    if (::inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr) != 1) {
        ::close(listen_fd_); listen_fd_ = -1;
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_); listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 16) < 0) {
        ::close(listen_fd_); listen_fd_ = -1;
        return false;
    }

    // Recover bound port (for bind_port=0 ephemeral flows).
    socklen_t alen = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    bound_port_ = ntohs(addr.sin_port);

    running_.store(true);
    acceptor_thread_ = std::thread(&Gateway::acceptor_loop, this);
    return true;
}

void Gateway::stop() {
    bool was = running_.exchange(false);
    if (!was) return;

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (acceptor_thread_.joinable()) acceptor_thread_.join();

    std::vector<std::shared_ptr<Session>> snapshot;
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        snapshot.swap(sessions_);
    }
    for (auto& s : snapshot) {
        s->running.store(false);
        if (s->fd >= 0) {
            ::shutdown(s->fd, SHUT_RDWR);
            ::close(s->fd);
            s->fd = -1;
        }
        if (s->worker.joinable()) s->worker.join();
    }
}

void Gateway::acceptor_loop() {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t   plen = sizeof(peer);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) {
            if (!running_.load()) break;
            if (errno == EINTR) continue;
            break;
        }
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        auto sess = std::make_shared<Session>();
        sess->fd = fd;
        accepted_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(sessions_mu_);
            sessions_.push_back(sess);
        }
        sess->worker = std::thread(&Gateway::session_loop, this, sess);
    }
}

void Gateway::session_loop(std::shared_ptr<Session> s) {
    char chunk[4096];
    while (s->running.load() && running_.load()) {
        ssize_t n = ::recv(s->fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        s->rx_buf.append(chunk, static_cast<std::size_t>(n));

        // Drain as many complete frames as possible.
        for (;;) {
            std::string_view frame;
            std::size_t consumed = 0;
            auto st = split_frame(s->rx_buf, &frame, &consumed);
            if (st == FrameStatus::Incomplete) break;
            if (st == FrameStatus::Malformed) {
                send_logout(*s, "malformed FIX frame");
                s->running.store(false);
                break;
            }
            Message m;
            if (!parse_frame(frame, &m)) {
                send_logout(*s, "parse failed");
                s->running.store(false);
                break;
            }
            ++s->in_seq;
            handle_message(*s, m);
            s->rx_buf.erase(0, consumed);
        }
    }
    if (s->fd >= 0) {
        ::shutdown(s->fd, SHUT_RDWR);
        ::close(s->fd);
        s->fd = -1;
    }
}

void Gateway::handle_message(Session& s, const Message& m) {
    char mt = m.msg_type();
    // Before logon, only accept A (logon).
    if (!s.logged_on && mt != 'A') {
        send_logout(s, "not logged on");
        s.running.store(false);
        return;
    }
    switch (mt) {
        case 'A': on_logon(s, m);      break;
        case '5': on_logout(s, m);     break;
        case '0': on_heartbeat(s, m);  break;
        case 'D': on_new_order(s, m);  break;
        case 'F': on_cancel(s, m);     break;
        case 'V': on_md_request(s, m); break;
        default:
            // Unknown / unsupported; ignore rather than drop session.
            break;
    }
}

void Gateway::on_logon(Session& s, const Message& m) {
    s.sender_comp = std::string(m.get(SenderCompID));
    s.logged_on = true;
    send_logon_ack(s);
}

void Gateway::on_logout(Session& s, const Message& /*m*/) {
    send_logout(s, "bye");
    s.running.store(false);
}

void Gateway::on_heartbeat(Session& /*s*/, const Message& /*m*/) {
    // No-op; a production impl would reset the heartbeat watchdog.
}

void Gateway::on_new_order(Session& s, const Message& m) {
    orders_.fetch_add(1);

    auto cl_ord_id = m.get(ClOrdID);
    auto symbol    = m.get(Symbol);
    char side_c    = !m.get(SideTag).empty() ? m.get(SideTag)[0] : '1';
    double order_qty = m.get_double(OrderQty);
    double price     = m.get_double(PriceTag);

    // Reject garbage before it hits the engine.
    if (order_qty <= 0 || price <= 0 || (side_c != '1' && side_c != '2')) {
        send_exec_report(s, cl_ord_id, 0, EX_REJECTED, OS_REJECTED,
                         side_c, symbol, order_qty, 0, 0, 0, 0, 0);
        return;
    }

    Side side = (side_c == '1') ? Side::Buy : Side::Sell;
    // Convert double price to integer ticks (2 decimal places → *100).
    Price px_ticks = static_cast<Price>(price * 100.0 + 0.5);
    Quantity qty   = static_cast<Quantity>(order_qty);

    OrderId order_id;
    std::vector<Trade> trades;
    Price bid_px, ask_px;
    Quantity bid_sz, ask_sz;
    {
        std::lock_guard<std::mutex> lk(engine_mu_);
        order_id = engine_.place_limit(side, px_ticks, qty);
        trades = engine_.last_trades();
        bid_px = engine_.book().best_bid();
        ask_px = engine_.book().best_ask();
        bid_sz = (bid_px > 0) ? engine_.book().qty_at(Side::Buy, bid_px) : 0;
        ask_sz = (ask_px < INT64_MAX) ?
                   engine_.book().qty_at(Side::Sell, ask_px) : 0;
    }

    // Emit one ExecutionReport per trade (PARTIAL_FILL / FILL) and a NEW
    // report for the accepted remainder (or initial NEW if no trades).
    Quantity filled = 0;
    for (auto const& t : trades) filled += t.qty;
    Quantity leaves = qty - filled;

    if (trades.empty()) {
        send_exec_report(s, cl_ord_id, order_id, EX_NEW, OS_NEW,
                         side_c, symbol,
                         static_cast<double>(qty), 0,
                         static_cast<double>(leaves),
                         0, 0, 0);
    } else {
        Quantity cum = 0;
        double notional = 0;
        for (std::size_t i = 0; i < trades.size(); ++i) {
            cum += trades[i].qty;
            double px = static_cast<double>(trades[i].price) / 100.0;
            notional += px * static_cast<double>(trades[i].qty);
            double avg = notional / static_cast<double>(cum);
            Quantity lv = qty - cum;
            bool final_fill = (i + 1 == trades.size()) && lv == 0;
            send_exec_report(s, cl_ord_id, order_id,
                             final_fill ? EX_FILL : EX_PARTIAL_FILL,
                             final_fill ? OS_FILLED : OS_PARTIAL_FILL,
                             side_c, symbol,
                             static_cast<double>(qty),
                             static_cast<double>(cum),
                             static_cast<double>(lv),
                             static_cast<double>(trades[i].qty),
                             px, avg);
        }
        if (leaves > 0) {
            // Remainder rested — one more NEW-style report for the resting qty.
            send_exec_report(s, cl_ord_id, order_id, EX_NEW,
                             OS_PARTIAL_FILL, side_c, symbol,
                             static_cast<double>(qty),
                             static_cast<double>(filled),
                             static_cast<double>(leaves),
                             0, 0, 0);
        }
    }

    // Publish L1 update on book change.
    if (feed_) {
        feed_->publish_l1(symbol,
                          static_cast<double>(bid_px) / 100.0,
                          static_cast<double>(bid_sz),
                          (ask_px < INT64_MAX)
                               ? static_cast<double>(ask_px) / 100.0 : 0.0,
                          static_cast<double>(ask_sz));
    }
}

void Gateway::on_cancel(Session& s, const Message& m) {
    auto cl_ord_id      = m.get(ClOrdID);
    auto orig_cl_ord_id = m.get(OrigClOrdID);
    OrderId oid = static_cast<OrderId>(m.get_int(OrderID));
    auto symbol = m.get(Symbol);
    char side_c = !m.get(SideTag).empty() ? m.get(SideTag)[0] : '1';

    bool ok;
    {
        std::lock_guard<std::mutex> lk(engine_mu_);
        ok = engine_.cancel(oid);
    }
    (void)orig_cl_ord_id;

    send_exec_report(s, cl_ord_id, oid,
                     ok ? EX_CANCELED : EX_REJECTED,
                     ok ? OS_CANCELED : OS_REJECTED,
                     side_c, symbol, 0, 0, 0, 0, 0, 0);
}

void Gateway::on_md_request(Session& s, const Message& m) {
    auto req_id = m.get(MDReqID);
    auto symbol = m.get(Symbol);
    // NoRelatedSym list parsing is skipped; we only support single-symbol
    // requests. We also ignore SubscriptionRequestType and always return a
    // one-shot snapshot (W). A live impl would register the subscription.

    Price bid_px, ask_px;
    Quantity bid_sz, ask_sz;
    {
        std::lock_guard<std::mutex> lk(engine_mu_);
        bid_px = engine_.book().best_bid();
        ask_px = engine_.book().best_ask();
        bid_sz = (bid_px > 0) ? engine_.book().qty_at(Side::Buy, bid_px) : 0;
        ask_sz = (ask_px < INT64_MAX) ?
                   engine_.book().qty_at(Side::Sell, ask_px) : 0;
    }
    send_md_snapshot(s, req_id, symbol,
                     static_cast<double>(bid_px) / 100.0,
                     static_cast<double>(bid_sz),
                     (ask_px < INT64_MAX)
                           ? static_cast<double>(ask_px) / 100.0 : 0.0,
                     static_cast<double>(ask_sz));
}

// -------- Outbound helpers --------------------------------------------------

void Gateway::send_raw(Session& s, std::string_view bytes) {
    const char* p = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        ssize_t n = ::send(s.fd, p, remaining, 0);
        if (n <= 0) { s.running.store(false); return; }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
}

void Gateway::send_logon_ack(Session& s) {
    s.builder.begin('A', cfg_.comp_id, s.sender_comp, s.out_seq++);
    s.builder.add_int(EncryptMethod, 0);
    s.builder.add_int(HeartBtInt, 30);
    send_raw(s, s.builder.finalize());
}

void Gateway::send_logout(Session& s, std::string_view text) {
    s.builder.begin('5', cfg_.comp_id, s.sender_comp, s.out_seq++);
    if (!text.empty()) s.builder.add_str(Text, text);
    send_raw(s, s.builder.finalize());
}

void Gateway::send_exec_report(Session& s,
                               std::string_view cl_ord_id,
                               uint64_t order_id,
                               char exec_type,
                               char ord_status,
                               char side,
                               std::string_view symbol,
                               double order_qty,
                               double cum_qty,
                               double leaves_qty,
                               double last_qty,
                               double last_px,
                               double avg_px) {
    uint64_t eid = next_exec_id_.fetch_add(1);
    char eid_buf[32];
    int en = std::snprintf(eid_buf, sizeof(eid_buf), "E%llu",
                           static_cast<unsigned long long>(eid));
    char oid_buf[32];
    int on = std::snprintf(oid_buf, sizeof(oid_buf), "%llu",
                           static_cast<unsigned long long>(order_id));

    s.builder.begin('8', cfg_.comp_id, s.sender_comp, s.out_seq++);
    s.builder.add_str(OrderID, std::string_view{oid_buf, (std::size_t)on});
    s.builder.add_str(ClOrdID, cl_ord_id);
    s.builder.add_str(ExecID,  std::string_view{eid_buf, (std::size_t)en});
    s.builder.add_char(ExecType, exec_type);
    s.builder.add_char(OrdStatus, ord_status);
    s.builder.add_str(Symbol, symbol);
    s.builder.add_char(SideTag, side);
    s.builder.add_double(OrderQty, order_qty, 0);
    s.builder.add_double(CumQty, cum_qty, 0);
    s.builder.add_double(LeavesQty, leaves_qty, 0);
    s.builder.add_double(LastQty, last_qty, 0);
    s.builder.add_double(LastPx, last_px, 2);
    s.builder.add_double(AvgPx, avg_px, 4);
    send_raw(s, s.builder.finalize());
    execs_.fetch_add(1);
}

void Gateway::send_md_snapshot(Session& s,
                               std::string_view md_req_id,
                               std::string_view symbol,
                               double bid_px, double bid_sz,
                               double ask_px, double ask_sz) {
    s.builder.begin('W', cfg_.comp_id, s.sender_comp, s.out_seq++);
    if (!md_req_id.empty()) s.builder.add_str(MDReqID, md_req_id);
    s.builder.add_str(Symbol, symbol);
    // Two MD entries: 0=Bid, 1=Offer.
    s.builder.add_int(NoMDEntries, 2);
    s.builder.add_char(MDEntryType, '0');
    s.builder.add_double(MDEntryPx, bid_px, 2);
    s.builder.add_double(MDEntrySize, bid_sz, 0);
    s.builder.add_char(MDEntryType, '1');
    s.builder.add_double(MDEntryPx, ask_px, 2);
    s.builder.add_double(MDEntrySize, ask_sz, 0);
    send_raw(s, s.builder.finalize());
}

} // namespace hft::fix
