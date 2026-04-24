#pragma once

// M6: sample FPGA driver stub.
//
// Purpose is to show where an FPGA-accelerated order book plugs in, not
// to be a working driver. Real integrations (e.g. Solarflare OpenOnload
// TCPDirect for AOE, or a Xilinx / Intel Agilex board running a custom
// matching bitstream) follow this shape:
//
//   - Open /dev/<vendor_fpga>N via a vendor char device.
//   - mmap() two rings: host -> FPGA (order submits) and FPGA -> host
//     (trade notifications, book deltas, reject reasons).
//   - Write a "doorbell" MMIO register after producing an SQE so the
//     FPGA picks it up without a syscall.
//   - Optionally read a PTP-synchronized 64-bit hardware timestamp out
//     of BAR0 to stamp every fill at nanosecond precision.
//
// In this build FpgaDriver::start() returns false unless HFT_FPGA=ON AND
// the configured device node exists, so the factory falls through to
// DPDK / software on normal dev boxes.

#include "../core/driver.h"

#include <atomic>

namespace hft::fpga {

struct FpgaConfig {
    std::string device     = "/dev/hft_fpga0";
    std::size_t ring_depth = 4096;          // host<->FPGA SPSC ring
    bool        use_hw_timestamp = true;    // read BAR0 PHC on upcall
};

class FpgaDriver final : public core::IKernelDriver {
public:
    FpgaDriver(FpgaConfig cfg, hft::Engine& engine);
    ~FpgaDriver() override;

    bool start() override;
    void stop() override;
    std::size_t poll(std::size_t budget = 64) override;
    void flush_tx() override;
    const core::DriverStats& stats() const override { return stats_; }
    std::string describe() const override;

    static constexpr bool native_fpga() {
#if defined(HFT_FPGA)
        return true;
#else
        return false;
#endif
    }

private:
    FpgaConfig          cfg_;
    hft::Engine*        engine_;
    core::DriverStats   stats_;
    std::atomic<bool>   running_{false};
    int                 fd_{-1};
    void*               bar_mmio_{nullptr};
    std::size_t         bar_len_{0};
};

} // namespace hft::fpga
