#pragma once

// FIX 4.4 TCP gateway. Accepts multiple sessions, parses inbound messages
// via hft::fix::parser, dispatches NewOrderSingle / OrderCancelRequest /
// MarketDataRequest into an hft::Engine, and emits ExecutionReports back to
// the originating session. When an order causes a book change, it publishes
// an L1 snapshot via the injected hft::md::Feed.
//
// Threading: one acceptor thread, plus one worker thread per accepted
// connection. The engine is protected by a coarse mutex — this is fine for
// M3 (we're proving the protocol layer, not scaling matching throughput).
// M4/M5 will route orders through the SPSC ring into a pinned matcher thread.

#include "parser.h"
#include "../engine.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hft::md { class Feed; }

namespace hft::fix {

struct GatewayConfig {
    std::string bind_host = "127.0.0.1";
    uint16_t    bind_port = 9878;
    std::string comp_id   = "ENGINE";      // our SenderCompID
};

class Gateway {
public:
    Gateway(GatewayConfig cfg, Engine& engine, md::Feed* feed = nullptr);
    ~Gateway();

    Gateway(const Gateway&)            = delete;
    Gateway& operator=(const Gateway&) = delete;

    // Start the acceptor thread. Returns false on bind/listen failure.
    bool start();

    // Stop + join all threads. Safe to call multiple times.
    void stop();

    // Actual bound port (useful when config requested 0 for ephemeral).
    uint16_t bound_port() const { return bound_port_; }

    // Stats (atomic snapshots, fine for lightweight monitoring).
    uint64_t accepted_sessions() const { return accepted_.load(); }
    uint64_t orders_received()   const { return orders_.load(); }
    uint64_t exec_reports_sent() const { return execs_.load(); }

private:
    // Per-session state. Accessed from one worker thread + the acceptor's
    // shutdown path, so the fd is atomic.
    struct Session {
        int                    fd{-1};
        std::string            sender_comp;   // counterparty CompID (their 49)
        uint32_t               out_seq{1};
        uint32_t               in_seq{0};
        bool                   logged_on{false};
        Builder                builder;
        std::string            rx_buf;
        std::thread            worker;
        std::atomic<bool>      running{true};
    };

    void acceptor_loop();
    void session_loop(std::shared_ptr<Session> s);
    void handle_message(Session& s, const Message& m);

    // Protocol handlers.
    void on_logon      (Session& s, const Message& m);
    void on_logout     (Session& s, const Message& m);
    void on_heartbeat  (Session& s, const Message& m);
    void on_new_order  (Session& s, const Message& m);
    void on_cancel     (Session& s, const Message& m);
    void on_md_request (Session& s, const Message& m);

    // Outbound helpers.
    void send_raw        (Session& s, std::string_view bytes);
    void send_logon_ack  (Session& s);
    void send_logout     (Session& s, std::string_view text = {});
    void send_exec_report(Session& s,
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
                          double avg_px);
    void send_md_snapshot(Session& s,
                          std::string_view md_req_id,
                          std::string_view symbol,
                          double bid_px, double bid_sz,
                          double ask_px, double ask_sz);

    GatewayConfig cfg_;
    Engine&       engine_;
    md::Feed*     feed_;

    std::mutex    engine_mu_;    // guards engine_ across sessions
    int           listen_fd_{-1};
    uint16_t      bound_port_{0};
    std::atomic<bool> running_{false};

    std::thread   acceptor_thread_;
    std::mutex    sessions_mu_;
    std::vector<std::shared_ptr<Session>> sessions_;

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> orders_{0};
    std::atomic<uint64_t> execs_{0};
    std::atomic<uint64_t> next_exec_id_{1};
};

} // namespace hft::fix
