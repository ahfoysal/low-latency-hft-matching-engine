#pragma once

// M6: hardware-abstracted matching-kernel driver.
//
// The matching core doesn't care whether its inputs arrive from a POSIX
// socket, an io_uring ring, a DPDK poll-mode driver, or an FPGA PCIe BAR.
// `IKernelDriver` is the trait that decouples the transport from the
// engine. Each implementation is responsible for:
//
//   1. Receiving wire-format order events from some source (socket, NIC
//      queue, FPGA DMA ring, ...).
//   2. Decoding them into hft::Engine calls.
//   3. Publishing ExecutionReports / book deltas back out.
//
// The engine itself stays single-threaded and lock-free; the driver owns
// threading, batching, and memory placement.
//
// Implementations provided in M6:
//
//   - SoftwareDriver  (src/core/software_driver.{h,cpp})
//       Wraps the M3 FIX gateway + M5 UringGateway. This is the default
//       and the only driver built everywhere.
//
//   - DpdkDriver      (src/dpdk/dpdk_driver.{h,cpp})
//       Poll-mode on a DPDK-bound NIC queue. Compiled only when
//       HFT_DPDK=ON and librte_eal is findable. On macOS it's a stub.
//
//   - FpgaDriver      (src/fpga/fpga_driver.{h,cpp})
//       Sample stub that shows how an FPGA-accelerated order book
//       would plug in — memory-mapped BAR, 1 ring per direction,
//       doorbell via a single MMIO store. Returns "not available"
//       unless HFT_FPGA=ON and a mock device file is present.

#include "../engine.h"

#include <cstdint>
#include <memory>
#include <string>

namespace hft::core {

// Steady-clock nanoseconds since some epoch (cycle-counter derived on the
// hot path; wall-clock on the slow path). Passed through the driver so
// FPGA hardware timestamps survive upcall.
using TimestampNs = uint64_t;

struct DriverStats {
    uint64_t rx_messages        = 0;
    uint64_t tx_messages        = 0;
    uint64_t rx_bytes           = 0;
    uint64_t tx_bytes           = 0;
    uint64_t decode_errors      = 0;
    uint64_t engine_rejects     = 0;
    // ISA / transport label for logs + /proc-style dump.
    std::string backend         = "unknown";
};

// The driver trait. Implementations must be non-blocking on `poll()` and
// cheap to call — the run-loop may spin on it.
class IKernelDriver {
public:
    virtual ~IKernelDriver() = default;

    // One-time setup: bind sockets, claim NIC queues, mmap FPGA BAR, etc.
    // Returns false on unrecoverable init failure; the caller may fall
    // back to a different driver.
    virtual bool start() = 0;

    // Cooperative shutdown. Safe to call from another thread.
    virtual void stop() = 0;

    // Non-blocking drain. Called from a tight run-loop on the matcher
    // core. Returns the number of engine ops produced by this call so
    // the caller can back off when the pipe is idle.
    //
    // `budget` caps the amount of work in a single poll so one fat
    // client can't starve others.
    virtual std::size_t poll(std::size_t budget = 64) = 0;

    // Optional hook: the driver may batch its publisher output and only
    // flush on an explicit tick. The software driver ignores this; DPDK
    // uses it to coalesce IORING_OP_SEND / rte_eth_tx_burst.
    virtual void flush_tx() {}

    virtual const DriverStats& stats() const = 0;

    // Human-readable one-liner for README / --version output.
    virtual std::string describe() const = 0;
};

// Factory helpers. Each returns nullptr on platforms where the backend
// is unavailable (e.g. DpdkDriver on macOS without HFT_DPDK, FpgaDriver
// with no device node). Callers are expected to try them in priority
// order: fpga -> dpdk -> software.
struct DriverFactoryConfig {
    std::string bind_host = "127.0.0.1";
    uint16_t    bind_port = 9878;
    std::string comp_id   = "ENGINE";

    // DPDK / FPGA specific knobs (ignored by software driver).
    std::string dpdk_eal_args = "";   // raw argv-style EAL string
    uint16_t    dpdk_port_id  = 0;
    uint16_t    dpdk_queue_id = 0;
    std::string fpga_device   = "/dev/hft_fpga0";
};

std::unique_ptr<IKernelDriver> make_software_driver(
    const DriverFactoryConfig&, Engine&);

std::unique_ptr<IKernelDriver> make_dpdk_driver(
    const DriverFactoryConfig&, Engine&);

std::unique_ptr<IKernelDriver> make_fpga_driver(
    const DriverFactoryConfig&, Engine&);

// Try all drivers in hardware-accelerated-first order. Never returns
// nullptr because the software driver is always available.
std::unique_ptr<IKernelDriver> make_best_available_driver(
    const DriverFactoryConfig&, Engine&);

} // namespace hft::core
