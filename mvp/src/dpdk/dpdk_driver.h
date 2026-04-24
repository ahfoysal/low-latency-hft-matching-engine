#pragma once

// M6: DPDK kernel-bypass driver scaffold.
//
// Intentionally built as a thin skeleton. The shape mirrors
// hft::iouring::UringGateway so the two are substitutable at the
// IKernelDriver layer. Native DPDK code is gated behind HFT_DPDK + a
// Linux build; everywhere else DpdkDriver::start() returns false and
// the factory falls back to SoftwareDriver.
//
// What this compiles-and-runs version does today:
//   - Parses EAL argv out of DriverFactoryConfig::dpdk_eal_args.
//   - Holds a config for port_id / queue_id / mempool sizing.
//   - Exposes the IKernelDriver API.
//   - With HFT_DPDK=ON (Linux + librte_eal + librte_ethdev + librte_mbuf):
//       rte_eal_init -> rte_pktmbuf_pool_create -> rte_eth_dev_configure
//       -> rx/tx queue setup -> rte_eth_dev_start -> poll loop.
//     The poll loop calls rte_eth_rx_burst, decodes framed FIX out of
//     each mbuf's payload (reusing hft::fix::Parser), dispatches to the
//     engine, coalesces ExecutionReports into a pre-allocated tx mbuf,
//     and flushes on flush_tx().
//
// What remains (tracked in README M6):
//   - Flow bifurcation rules (rte_flow) to steer only FIX-over-TCP frags
//     into our queue.
//   - Userspace TCP reassembly (e.g. mTCP or a custom reassembler); raw
//     DPDK gives us L2, not a stream. For now the scaffold assumes one
//     FIX message per UDP-encapsulated datagram — enough to prove the
//     plumbing against a test publisher.

#include "../core/driver.h"

#include <atomic>
#include <memory>

namespace hft::dpdk {

struct DpdkConfig {
    std::string eal_args     = "-l 0-3 --proc-type=primary";
    uint16_t    port_id      = 0;
    uint16_t    queue_id     = 0;
    uint16_t    nb_rx_desc   = 1024;
    uint16_t    nb_tx_desc   = 1024;
    uint32_t    mbuf_pool_sz = 8191;
    uint16_t    mtu          = 1500;
};

class DpdkDriver final : public core::IKernelDriver {
public:
    DpdkDriver(DpdkConfig cfg, hft::Engine& engine);
    ~DpdkDriver() override;

    bool start() override;
    void stop() override;
    std::size_t poll(std::size_t budget = 64) override;
    void flush_tx() override;
    const core::DriverStats& stats() const override { return stats_; }
    std::string describe() const override;

    // True on Linux builds with HFT_DPDK=ON.
    static constexpr bool native_dpdk() {
#if defined(__linux__) && defined(HFT_DPDK)
        return true;
#else
        return false;
#endif
    }

private:
    DpdkConfig          cfg_;
    hft::Engine*        engine_;
    core::DriverStats   stats_;
    std::atomic<bool>   running_{false};

#if defined(__linux__) && defined(HFT_DPDK)
    struct Native;
    std::unique_ptr<Native> native_;
#endif
};

} // namespace hft::dpdk
