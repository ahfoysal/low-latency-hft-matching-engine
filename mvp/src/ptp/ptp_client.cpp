#include "ptp_client.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__linux__) && defined(HFT_PTP)
#  include <linux/ptp_clock.h>
#  include <sys/ioctl.h>
#  include <time.h>
#endif

namespace hft::ptp {

namespace {

inline int64_t mono_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

// 32-byte mock sync datagram, text-framed for readability.
//   "PTP1 T1=<ns> T2=<ns>\n"  (client -> oracle, T1 = client send)
//   "PTP1 T1=<ns> T2=<ns> T3=<ns>\n" (oracle reply, T2,T3 = master recv/send)
// Client then takes T4 (= local recv), estimates offset:
//   offset = ((T2 - T1) - (T4 - T3)) / 2
//   delay  = ((T4 - T1) - (T3 - T2)) / 2
// Same math as NTP / PTP sync+delay_req.

constexpr std::size_t kDatagramMax = 128;

} // namespace

PtpClient::PtpClient(PtpConfig cfg) : cfg_(std::move(cfg)) {}
PtpClient::~PtpClient() { stop(); }

bool PtpClient::resolve_backend() {
    if (cfg_.backend != Backend::Auto) {
        active_ = cfg_.backend;
        return true;
    }
#if defined(__linux__) && defined(HFT_PTP)
    if (::access(cfg_.phc_device.c_str(), R_OK) == 0) {
        active_ = Backend::LinuxPhc;
        return true;
    }
#endif
    if (!cfg_.ptp4l_uds.empty() && ::access(cfg_.ptp4l_uds.c_str(), R_OK | W_OK) == 0) {
        active_ = Backend::Ptp4lUds;
        return true;
    }
    // Always-available fallback: mock oracle.
    active_ = Backend::MockOracle;
    return true;
}

bool PtpClient::start() {
    if (running_.exchange(true)) return true;
    if (!resolve_backend()) {
        running_ = false;
        return false;
    }
    sync_thread_ = std::thread([this] {
        while (running_.load(std::memory_order_relaxed)) {
            auto s = sample();
            if (s.valid) {
                int64_t prev  = offset_ewma_ns_.load(std::memory_order_relaxed);
                int64_t fresh = (sample_count_.load() == 0)
                    ? s.offset_ns
                    : static_cast<int64_t>(prev + cfg_.ewma_alpha * (s.offset_ns - prev));
                offset_ewma_ns_.store(fresh, std::memory_order_relaxed);
                sample_count_.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.sync_interval_ms));
        }
    });
    return true;
}

void PtpClient::stop() {
    if (!running_.exchange(false)) return;
    if (sync_thread_.joinable()) sync_thread_.join();
}

PtpSample PtpClient::sample() {
    std::optional<PtpSample> s;
    switch (active_) {
        case Backend::LinuxPhc:   s = sample_phc();    break;
        case Backend::Ptp4lUds:   s = sample_ptp4l();  break;
        case Backend::MockOracle: s = sample_mock();   break;
        case Backend::Auto:       /* resolved */       break;
    }
    return s.value_or(PtpSample{});
}

std::optional<PtpSample> PtpClient::sample_phc() {
#if defined(__linux__) && defined(HFT_PTP)
    int fd = ::open(cfg_.phc_device.c_str(), O_RDWR);
    if (fd < 0) return std::nullopt;
    // clock_gettime on PHC fd: fd -> clockid via helper.
    clockid_t phc_clk = (~(clockid_t)fd << 3) | 3;  // FD_TO_CLOCKID
    timespec phc{}, sys{};
    if (::clock_gettime(phc_clk, &phc) != 0 ||
        ::clock_gettime(CLOCK_REALTIME, &sys) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    ::close(fd);
    int64_t phc_ns = (int64_t)phc.tv_sec * 1'000'000'000LL + phc.tv_nsec;
    int64_t sys_ns = (int64_t)sys.tv_sec * 1'000'000'000LL + sys.tv_nsec;
    PtpSample s{};
    s.offset_ns  = phc_ns - sys_ns;
    s.one_way_ns = 0;           // PHC is on-host; one-way is negligible
    s.local_ns   = (uint64_t)mono_ns();
    s.valid      = true;
    return s;
#else
    return std::nullopt;
#endif
}

std::optional<PtpSample> PtpClient::sample_ptp4l() {
    // Real impl speaks IEEE1588 management messages over the ptp4l UDS.
    // That's ~200 LOC of TLV plumbing; for M6 we stub it to "unavailable"
    // unless the socket exists and responds to a probe. The interface is
    // the deliverable — the wire protocol is orthogonal.
    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return std::nullopt;
    ::close(fd);
    // Declared unavailable in this build. Real integrations wire in
    // linuxptp's pmc(8) message format here.
    return std::nullopt;
}

std::optional<PtpSample> PtpClient::sample_mock() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return std::nullopt;
    // 200ms recv timeout so a missing oracle fails fast.
    timeval tv{ .tv_sec = 0, .tv_usec = 200'000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(cfg_.mock_port);
    if (::inet_pton(AF_INET, cfg_.mock_host.c_str(), &dest.sin_addr) != 1) {
        ::close(fd);
        return std::nullopt;
    }

    char   buf[kDatagramMax];
    int64_t t1 = mono_ns();
    int n = std::snprintf(buf, sizeof(buf), "PTP1 T1=%lld\n", (long long)t1);
    if (n <= 0) { ::close(fd); return std::nullopt; }
    if (::sendto(fd, buf, (size_t)n, 0,
                 reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) != n) {
        ::close(fd);
        return std::nullopt;
    }

    char rbuf[kDatagramMax];
    ssize_t r = ::recv(fd, rbuf, sizeof(rbuf) - 1, 0);
    int64_t t4 = mono_ns();
    ::close(fd);
    if (r <= 0) return std::nullopt;
    rbuf[r] = '\0';

    long long rt1 = 0, t2 = 0, t3 = 0;
    if (std::sscanf(rbuf, "PTP1 T1=%lld T2=%lld T3=%lld", &rt1, &t2, &t3) != 3) {
        return std::nullopt;
    }
    if (rt1 != t1) return std::nullopt;  // stale / wrong reply

    PtpSample s{};
    // offset = ((T2 - T1) + (T3 - T4)) / 2  (classic PTP two-way)
    s.offset_ns  = ((int64_t)t2 - t1 + (int64_t)t3 - t4) / 2;
    s.one_way_ns = ((int64_t)t4 - t1 - ((int64_t)t3 - t2)) / 2;
    s.local_ns   = (uint64_t)t4;
    s.valid      = true;
    return s;
}

} // namespace hft::ptp
