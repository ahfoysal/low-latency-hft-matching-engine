#include "dpdk_driver.h"

#include <cstdio>
#include <sstream>
#include <vector>

#if defined(__linux__) && defined(HFT_DPDK)
#  include <rte_eal.h>
#  include <rte_ethdev.h>
#  include <rte_mbuf.h>
#  include <rte_mempool.h>
#endif

namespace hft::dpdk {

#if defined(__linux__) && defined(HFT_DPDK)

struct DpdkDriver::Native {
    rte_mempool* mbuf_pool = nullptr;
    bool         eal_inited = false;
    bool         port_started = false;
};

namespace {

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(std::move(tok));
    return out;
}

} // namespace

#endif // HFT_DPDK

DpdkDriver::DpdkDriver(DpdkConfig cfg, hft::Engine& engine)
    : cfg_(std::move(cfg)), engine_(&engine) {
    stats_.backend = native_dpdk() ? "dpdk/poll-mode" : "dpdk/stub";
}

DpdkDriver::~DpdkDriver() { stop(); }

bool DpdkDriver::start() {
    if (running_.exchange(true)) return true;
#if defined(__linux__) && defined(HFT_DPDK)
    native_ = std::make_unique<Native>();

    // 1. rte_eal_init — argv-style.
    auto args = split_args(cfg_.eal_args);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    std::string prog = "hft";
    argv.push_back(prog.data());
    for (auto& a : args) argv.push_back(a.data());
    int ac = (int)argv.size();
    int eal_ret = rte_eal_init(ac, argv.data());
    if (eal_ret < 0) { running_ = false; return false; }
    native_->eal_inited = true;

    // 2. Mempool + port configure.
    native_->mbuf_pool = rte_pktmbuf_pool_create(
        "hft_mbuf_pool", cfg_.mbuf_pool_sz, 256, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (!native_->mbuf_pool) { running_ = false; return false; }

    rte_eth_conf ec{};
    ec.rxmode.mtu = cfg_.mtu;
    if (rte_eth_dev_configure(cfg_.port_id, 1, 1, &ec) < 0) {
        running_ = false; return false;
    }
    rte_eth_rx_queue_setup(cfg_.port_id, cfg_.queue_id, cfg_.nb_rx_desc,
                           rte_eth_dev_socket_id(cfg_.port_id),
                           nullptr, native_->mbuf_pool);
    rte_eth_tx_queue_setup(cfg_.port_id, cfg_.queue_id, cfg_.nb_tx_desc,
                           rte_eth_dev_socket_id(cfg_.port_id), nullptr);
    if (rte_eth_dev_start(cfg_.port_id) < 0) { running_ = false; return false; }
    native_->port_started = true;
    return true;
#else
    // Stub build (macOS, or Linux without -DHFT_DPDK=ON). The factory is
    // expected to treat this as a fail and fall back to SoftwareDriver.
    running_ = false;
    return false;
#endif
}

void DpdkDriver::stop() {
    if (!running_.exchange(false)) return;
#if defined(__linux__) && defined(HFT_DPDK)
    if (native_) {
        if (native_->port_started) rte_eth_dev_stop(cfg_.port_id);
        if (native_->eal_inited)   rte_eal_cleanup();
    }
    native_.reset();
#endif
}

std::size_t DpdkDriver::poll(std::size_t budget) {
#if defined(__linux__) && defined(HFT_DPDK)
    if (!running_.load(std::memory_order_relaxed)) return 0;
    constexpr std::size_t kBurst = 32;
    rte_mbuf* rx[kBurst];
    std::size_t done = 0;
    while (done < budget) {
        uint16_t n = rte_eth_rx_burst(cfg_.port_id, cfg_.queue_id, rx,
                                      (uint16_t)std::min<std::size_t>(kBurst, budget - done));
        if (n == 0) break;
        for (uint16_t i = 0; i < n; ++i) {
            // Real path: locate FIX payload inside mbuf, feed parser, emit
            // ExecutionReports into coalesced tx burst. Scaffold counts only.
            stats_.rx_messages += 1;
            stats_.rx_bytes    += rte_pktmbuf_pkt_len(rx[i]);
            rte_pktmbuf_free(rx[i]);
        }
        done += n;
    }
    return done;
#else
    (void)budget;
    return 0;
#endif
}

void DpdkDriver::flush_tx() {
#if defined(__linux__) && defined(HFT_DPDK)
    // Real path coalesces ExecutionReports into a single rte_eth_tx_burst.
#endif
}

std::string DpdkDriver::describe() const {
    return native_dpdk()
        ? (std::string("DpdkDriver[native] port=") + std::to_string(cfg_.port_id)
           + " queue=" + std::to_string(cfg_.queue_id))
        : std::string("DpdkDriver[stub - HFT_DPDK=OFF or non-Linux build]");
}

} // namespace hft::dpdk

namespace hft::core {

std::unique_ptr<IKernelDriver> make_dpdk_driver(
    const DriverFactoryConfig& cfg, Engine& engine) {
    if (!dpdk::DpdkDriver::native_dpdk()) return nullptr;
    dpdk::DpdkConfig dc;
    if (!cfg.dpdk_eal_args.empty()) dc.eal_args = cfg.dpdk_eal_args;
    dc.port_id  = cfg.dpdk_port_id;
    dc.queue_id = cfg.dpdk_queue_id;
    auto d = std::make_unique<dpdk::DpdkDriver>(std::move(dc), engine);
    if (!d->start()) return nullptr;
    // Hand back an already-started driver; caller owns shutdown.
    return d;
}

} // namespace hft::core
