#pragma once

// M6: software event-loop driver.
//
// Wraps the M3 FIX gateway + M5 UringGateway (which itself falls back to
// the POSIX FIX gateway on macOS). This is the always-available driver —
// no kernel-bypass, no FPGA, just sockets.

#include "driver.h"
#include "../iouring/uring_gateway.h"

#include <atomic>
#include <memory>

namespace hft::md { class Feed; }

namespace hft::core {

class SoftwareDriver final : public IKernelDriver {
public:
    SoftwareDriver(const DriverFactoryConfig& cfg, Engine& engine);
    ~SoftwareDriver() override;

    bool start() override;
    void stop() override;
    std::size_t poll(std::size_t budget = 64) override;
    void flush_tx() override;
    const DriverStats& stats() const override { return stats_; }
    std::string describe() const override;

private:
    Engine*                                     engine_;
    DriverFactoryConfig                         cfg_;
    std::unique_ptr<iouring::UringGateway>      gateway_;
    std::unique_ptr<md::Feed>                   feed_;
    DriverStats                                 stats_;
    std::atomic<bool>                           running_{false};
};

} // namespace hft::core
