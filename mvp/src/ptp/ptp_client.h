#pragma once

// M6: PTP (IEEE 1588) clock-sync client.
//
// Three backends, selected at runtime:
//
//   1. Linux PHC (`/dev/ptp0`):
//        Read the NIC's PTP hardware clock via clock_gettime(CLOCK_PTP).
//        Offset = PHC_now - system_now (after bias). Requires HFT_PTP=ON
//        and linux/ptp_clock.h at build time.
//
//   2. ptp4l UDS (`/var/run/ptp4l`):
//        Speak the management-message protocol over a Unix socket to an
//        already-running ptp4l daemon. Used when the matcher doesn't own
//        the NIC directly (common in colo).
//
//   3. Mock oracle (default on macOS / CI):
//        A second process (see ptp_oracle.cpp) that serves a compact
//        UDP datagram containing its own monotonic ns clock. The client
//        does a simple NTP-style round-trip and estimates offset +
//        one-way delay. Not sub-ns, but exercises the same code path.
//
// All three expose the same API:
//
//   PtpClient pc(cfg);
//   pc.start();
//   auto s = pc.sample();       // blocking-ish, does one sync exchange
//   auto off = pc.offset_ns();  // cached EWMA of recent samples
//
// The engine uses offset_ns() to stamp fills with a master-clock nanosecond
// so downstream risk / regulators see a consistent time base across hosts.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace hft::ptp {

enum class Backend {
    Auto,       // try phc -> ptp4l -> mock
    LinuxPhc,
    Ptp4lUds,
    MockOracle,
};

struct PtpConfig {
    Backend     backend          = Backend::Auto;
    std::string phc_device       = "/dev/ptp0";
    std::string ptp4l_uds        = "/var/run/ptp4l";
    // Mock oracle endpoint. See ptp_oracle.cpp.
    std::string mock_host        = "127.0.0.1";
    uint16_t    mock_port        = 9881;
    uint32_t    sync_interval_ms = 250;
    double      ewma_alpha       = 0.125;   // NTP classic
};

struct PtpSample {
    int64_t  offset_ns      = 0;   // master - local
    int64_t  one_way_ns     = 0;   // estimated one-way delay
    uint64_t local_ns       = 0;   // CLOCK_MONOTONIC_RAW ns at sample
    bool     valid          = false;
};

class PtpClient {
public:
    explicit PtpClient(PtpConfig cfg);
    ~PtpClient();

    PtpClient(const PtpClient&)            = delete;
    PtpClient& operator=(const PtpClient&) = delete;

    // Starts a background sync thread. Returns false if no backend could
    // be brought up (e.g. Auto on a box with neither /dev/ptp0 nor a
    // reachable mock oracle).
    bool start();
    void stop();

    // Best-effort single exchange. Safe to call from any thread.
    // On failure `valid` is false.
    PtpSample sample();

    // Cached EWMA offset in nanoseconds (master - local). Zero if no
    // valid sample has been collected yet.
    int64_t offset_ns() const { return offset_ewma_ns_.load(std::memory_order_relaxed); }

    // Number of successful samples so far.
    uint64_t sample_count() const { return sample_count_.load(std::memory_order_relaxed); }

    // Which backend is actually in use (after Auto resolution).
    Backend active_backend() const { return active_; }

    // Convert a local monotonic ns to estimated master ns.
    int64_t to_master_ns(int64_t local_ns) const {
        return local_ns + offset_ns();
    }

private:
    // Per-backend exchange. Each returns a valid sample on success.
    std::optional<PtpSample> sample_phc();
    std::optional<PtpSample> sample_ptp4l();
    std::optional<PtpSample> sample_mock();

    bool resolve_backend();

    PtpConfig                 cfg_;
    Backend                   active_{Backend::MockOracle};
    std::atomic<int64_t>      offset_ewma_ns_{0};
    std::atomic<uint64_t>     sample_count_{0};
    std::atomic<bool>         running_{false};
    std::thread               sync_thread_;
};

} // namespace hft::ptp
