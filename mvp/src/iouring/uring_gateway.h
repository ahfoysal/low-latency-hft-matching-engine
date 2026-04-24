#pragma once

// io_uring-based FIX gateway (Linux).
//
// Design notes:
//   - One io_uring instance per acceptor thread. SQ_POLL would eliminate the
//     io_uring_enter syscall entirely; we don't enable it by default because
//     it burns a full CPU even when idle — opt in via UringGatewayConfig.
//   - Per-connection state holds a registered read buffer (zero-copy path
//     via IORING_OP_READ into a pre-pinned buffer group) and a per-session
//     FIX parser.
//   - Writes use IORING_OP_SEND with MSG_WAITALL semantics by coalescing in
//     a per-session tx buffer and chaining a single SQE per flush.
//   - We do NOT shell out to liburing.h if HFT_IOURING is off — the whole
//     translation unit becomes a platform-neutral fallback that reuses the
//     existing POSIX hft::fix::Gateway. That keeps macOS / CI builds green.
//
// macOS fallback:
//   kqueue or plain blocking POSIX sockets are sufficient for correctness;
//   we reuse the hft::fix::Gateway implementation from M3 verbatim and
//   expose it under the UringGateway name with the same API. Benchmarks in
//   M5 still capture Linux numbers via QEMU/CI; the macOS path is just for
//   builds + local dev.

#include "../engine.h"
#include "../fix/gateway.h"

#include <cstdint>
#include <memory>
#include <string>

namespace hft::md { class Feed; }

namespace hft::iouring {

struct UringGatewayConfig {
    std::string bind_host = "127.0.0.1";
    uint16_t    bind_port = 9878;
    std::string comp_id   = "ENGINE";

    // Linux-only. Ignored on macOS.
    uint32_t    sq_entries       = 256;    // SQ ring depth
    bool        use_sq_poll      = false;  // IORING_SETUP_SQPOLL (busy-polls)
    uint32_t    sq_poll_idle_ms  = 2;      // poller sleeps after this idle window
    bool        register_buffers = true;   // pre-register RX buffer pool
    uint32_t    rx_buffer_count  = 64;     // buffers per ring
    uint32_t    rx_buffer_size   = 4096;
};

// Public API is the same on every platform. On Linux with HFT_IOURING=ON
// this is the real uring-backed impl; everywhere else it delegates to the
// POSIX hft::fix::Gateway from M3.
class UringGateway {
public:
    UringGateway(UringGatewayConfig cfg, Engine& engine, md::Feed* feed = nullptr);
    ~UringGateway();

    UringGateway(const UringGateway&)            = delete;
    UringGateway& operator=(const UringGateway&) = delete;

    bool start();
    void stop();

    uint16_t bound_port()        const;
    uint64_t accepted_sessions() const;
    uint64_t orders_received()   const;
    uint64_t exec_reports_sent() const;

    // True if this build really uses io_uring (Linux + HFT_IOURING=ON);
    // false means the POSIX fallback is in effect.
    static constexpr bool native_iouring() {
#if defined(__linux__) && defined(HFT_IOURING)
        return true;
#else
        return false;
#endif
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hft::iouring
