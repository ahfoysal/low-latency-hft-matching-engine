// M6 unit tests:
//   - IKernelDriver factory resolves to SoftwareDriver on macOS / no-DPDK builds.
//   - DpdkDriver and FpgaDriver factories return nullptr unless their
//     HFT_* build flag is on (we assert the "unavailable" side here;
//     positive paths need real hardware / CI runners).
//   - PtpClient mock oracle round-trip converges on the expected offset.

#include "../src/core/driver.h"
#include "../src/core/software_driver.h"
#include "../src/dpdk/dpdk_driver.h"
#include "../src/fpga/fpga_driver.h"
#include "../src/ptp/ptp_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace hft;

static int g_fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_fails; \
    } else { std::fprintf(stderr, "  ok   %s\n", #cond); } \
} while (0)

static int64_t mono_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

// Tiny in-process oracle for the PTP mock path. Applies a deterministic
// +1,234,567 ns offset so the client can assert convergence.
struct MockOracle {
    int fd{-1};
    uint16_t port{0};
    std::thread th;
    std::atomic<bool> run{true};

    bool start() {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        me.sin_port = 0;
        if (::bind(fd, (sockaddr*)&me, sizeof(me)) != 0) return false;
        socklen_t len = sizeof(me);
        ::getsockname(fd, (sockaddr*)&me, &len);
        port = ntohs(me.sin_port);
        th = std::thread([this]{
            char buf[256];
            while (run.load()) {
                timeval tv{0, 50'000};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                sockaddr_in peer{};
                socklen_t pl = sizeof(peer);
                ssize_t n = ::recvfrom(fd, buf, sizeof(buf)-1, 0,
                                       (sockaddr*)&peer, &pl);
                int64_t t2 = mono_ns();
                if (n <= 0) continue;
                buf[n] = '\0';
                long long t1 = 0;
                if (std::sscanf(buf, "PTP1 T1=%lld", &t1) != 1) continue;
                const int64_t OFFSET = 1'234'567;
                int64_t t3 = mono_ns() + OFFSET;
                t2 += OFFSET;
                char out[256];
                int olen = std::snprintf(out, sizeof(out),
                    "PTP1 T1=%lld T2=%lld T3=%lld\n",
                    t1, (long long)t2, (long long)t3);
                if (olen > 0) {
                    ::sendto(fd, out, (size_t)olen, 0,
                             (sockaddr*)&peer, pl);
                }
            }
        });
        return true;
    }
    void stop() {
        run = false;
        if (th.joinable()) th.join();
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
};

static void test_driver_factory_shapes() {
    std::fprintf(stderr, "[driver_factory_shapes]\n");

    Engine eng;
    core::DriverFactoryConfig cfg;
    // Use a high port to avoid collisions.
    cfg.bind_port = 19879;

    auto sw = core::make_software_driver(cfg, eng);
    CHECK(sw != nullptr);
    CHECK(sw->describe().find("SoftwareDriver") != std::string::npos);

    // DPDK / FPGA unavailable on macOS / non-flag builds.
    auto dpdk = core::make_dpdk_driver(cfg, eng);
    if (dpdk::DpdkDriver::native_dpdk()) {
        // Native DPDK build: the factory may still fail on a box with no
        // NIC bound — treat as either nullptr or started-successfully.
        std::fprintf(stderr, "  info native DPDK build; factory=%s\n",
                     dpdk ? "ok" : "null");
    } else {
        CHECK(dpdk == nullptr);
    }

    auto fp = core::make_fpga_driver(cfg, eng);
    if (fpga::FpgaDriver::native_fpga()) {
        std::fprintf(stderr, "  info HFT_FPGA build; factory=%s\n",
                     fp ? "ok" : "null");
    } else {
        CHECK(fp == nullptr);
    }

    // best_available always returns something.
    auto best = core::make_best_available_driver(cfg, eng);
    CHECK(best != nullptr);
    std::fprintf(stderr, "  info best=%s\n", best->describe().c_str());
}

static void test_dpdk_stub_describe() {
    std::fprintf(stderr, "[dpdk_stub_describe]\n");
    Engine eng;
    dpdk::DpdkDriver d({}, eng);
    auto s = d.describe();
    CHECK(!s.empty());
    if (!dpdk::DpdkDriver::native_dpdk()) {
        CHECK(s.find("stub") != std::string::npos);
        CHECK(d.start() == false);   // stub can't start
    }
}

static void test_fpga_stub_describe() {
    std::fprintf(stderr, "[fpga_stub_describe]\n");
    Engine eng;
    fpga::FpgaDriver d({}, eng);
    auto s = d.describe();
    CHECK(!s.empty());
    if (!fpga::FpgaDriver::native_fpga()) {
        CHECK(s.find("stub") != std::string::npos);
        CHECK(d.start() == false);
    }
}

static void test_ptp_mock_converges() {
    std::fprintf(stderr, "[ptp_mock_converges]\n");
    MockOracle oracle;
    if (!oracle.start()) {
        std::fprintf(stderr, "  skip: could not bind loopback oracle\n");
        return;
    }

    ptp::PtpConfig cfg;
    cfg.backend          = ptp::Backend::MockOracle;
    cfg.mock_host        = "127.0.0.1";
    cfg.mock_port        = oracle.port;
    cfg.sync_interval_ms = 20;
    ptp::PtpClient pc(cfg);
    CHECK(pc.start());

    // Wait for EWMA to converge.
    for (int i = 0; i < 100; ++i) {
        if (pc.sample_count() >= 8) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    int64_t off = pc.offset_ns();
    std::fprintf(stderr, "  info samples=%llu offset_ns=%lld\n",
                 (unsigned long long)pc.sample_count(), (long long)off);

    CHECK(pc.sample_count() >= 4);
    // Master was +1,234,567 ns ahead. Allow generous slop for loopback
    // jitter — ~200 microseconds.
    CHECK(off > 1'000'000 && off < 1'500'000);

    // Conversion helper.
    int64_t mapped = pc.to_master_ns(1000);
    CHECK(mapped > 1'000'000);

    pc.stop();
    oracle.stop();
}

static void test_software_driver_lifecycle() {
    std::fprintf(stderr, "[software_driver_lifecycle]\n");
    Engine eng;
    core::DriverFactoryConfig cfg;
    cfg.bind_port = 19880;
    auto d = core::make_software_driver(cfg, eng);
    CHECK(d->start());
    CHECK(d->describe().find("SoftwareDriver") != std::string::npos);
    // poll() is non-blocking; should return 0 with no clients connected.
    CHECK(d->poll(8) == 0);
    d->stop();
}

int main() {
    std::fprintf(stderr, "M6: driver trait + PTP + DPDK scaffold + FPGA stub\n");
    test_driver_factory_shapes();
    test_dpdk_stub_describe();
    test_fpga_stub_describe();
    test_software_driver_lifecycle();
    test_ptp_mock_converges();
    if (g_fails == 0) {
        std::fprintf(stderr, "all M6 tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d M6 test(s) failed\n", g_fails);
    return 1;
}
