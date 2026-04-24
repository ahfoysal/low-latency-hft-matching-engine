#include "software_driver.h"
#include "../md/feed.h"

namespace hft::core {

SoftwareDriver::SoftwareDriver(const DriverFactoryConfig& cfg, Engine& engine)
    : engine_(&engine), cfg_(cfg) {
    stats_.backend = iouring::UringGateway::native_iouring()
        ? "software/io_uring"
        : "software/posix";
}

SoftwareDriver::~SoftwareDriver() { stop(); }

bool SoftwareDriver::start() {
    if (running_.exchange(true)) return true;

    md::FeedConfig fc{};
    fc.dest_host = "127.0.0.1";
    feed_ = std::make_unique<md::Feed>(fc);
    if (!feed_->open()) {
        // Non-fatal — we still serve FIX orders.
        feed_.reset();
    }

    iouring::UringGatewayConfig gcfg{};
    gcfg.bind_host = cfg_.bind_host;
    gcfg.bind_port = cfg_.bind_port;
    gcfg.comp_id   = cfg_.comp_id;
    gateway_ = std::make_unique<iouring::UringGateway>(gcfg, *engine_, feed_.get());
    if (!gateway_->start()) {
        running_ = false;
        return false;
    }
    return true;
}

void SoftwareDriver::stop() {
    if (!running_.exchange(false)) return;
    if (gateway_) gateway_->stop();
    gateway_.reset();
    if (feed_) feed_->close();
    feed_.reset();
}

std::size_t SoftwareDriver::poll(std::size_t /*budget*/) {
    // The software gateway runs its own per-session worker threads, so
    // poll() is a no-op — the driver trait is here for the DPDK/FPGA
    // case where a pinned run-loop drives the kernel.
    if (!gateway_) return 0;
    uint64_t rx  = gateway_->orders_received();
    uint64_t tx  = gateway_->exec_reports_sent();
    std::size_t delta = (rx > stats_.rx_messages) ? (rx - stats_.rx_messages) : 0;
    stats_.rx_messages = rx;
    stats_.tx_messages = tx;
    return delta;
}

void SoftwareDriver::flush_tx() {
    // Gateway flushes per-message today. Left as a hook for batched path.
}

std::string SoftwareDriver::describe() const {
    return std::string("SoftwareDriver[") + stats_.backend + "] on "
         + cfg_.bind_host + ":" + std::to_string(cfg_.bind_port);
}

std::unique_ptr<IKernelDriver> make_software_driver(
    const DriverFactoryConfig& cfg, Engine& engine) {
    return std::make_unique<SoftwareDriver>(cfg, engine);
}

std::unique_ptr<IKernelDriver> make_best_available_driver(
    const DriverFactoryConfig& cfg, Engine& engine) {
    // Hardware-accel-first: FPGA -> DPDK -> software.
    if (auto d = make_fpga_driver(cfg, engine)) return d;
    if (auto d = make_dpdk_driver(cfg, engine)) return d;
    return make_software_driver(cfg, engine);
}

} // namespace hft::core
