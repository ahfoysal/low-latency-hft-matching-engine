#include "fpga_driver.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft::fpga {

FpgaDriver::FpgaDriver(FpgaConfig cfg, hft::Engine& engine)
    : cfg_(std::move(cfg)), engine_(&engine) {
    stats_.backend = native_fpga() ? "fpga/mmio" : "fpga/stub";
    (void)engine_;
}

FpgaDriver::~FpgaDriver() { stop(); }

bool FpgaDriver::start() {
    if (running_.exchange(true)) return true;
#if defined(HFT_FPGA)
    fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) { running_ = false; return false; }

    struct stat st{};
    if (::fstat(fd_, &st) != 0 || st.st_size == 0) {
        ::close(fd_); fd_ = -1; running_ = false; return false;
    }
    bar_len_  = (std::size_t)st.st_size;
    bar_mmio_ = ::mmap(nullptr, bar_len_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
    if (bar_mmio_ == MAP_FAILED) {
        bar_mmio_ = nullptr;
        ::close(fd_); fd_ = -1; running_ = false; return false;
    }
    // Real impl: zero the submit/complete ring head/tail registers, arm
    // doorbell interrupt-coalescing, read board-id out of BAR0[0x00].
    return true;
#else
    running_ = false;
    return false;
#endif
}

void FpgaDriver::stop() {
    if (!running_.exchange(false)) return;
    if (bar_mmio_) { ::munmap(bar_mmio_, bar_len_); bar_mmio_ = nullptr; }
    if (fd_ >= 0)  { ::close(fd_); fd_ = -1; }
}

std::size_t FpgaDriver::poll(std::size_t budget) {
#if defined(HFT_FPGA)
    // Real impl: read completion-ring tail pointer from BAR0, drain up to
    // `budget` completions, dispatch trades into engine, bump consumer
    // head with a single MMIO store. Scaffold returns 0.
    (void)budget;
    return 0;
#else
    (void)budget;
    return 0;
#endif
}

void FpgaDriver::flush_tx() {
    // Real impl: write doorbell register (BAR0[0x40]) with producer index.
}

std::string FpgaDriver::describe() const {
    return native_fpga()
        ? (std::string("FpgaDriver[native] dev=") + cfg_.device)
        : std::string("FpgaDriver[stub - HFT_FPGA=OFF]");
}

} // namespace hft::fpga

namespace hft::core {

std::unique_ptr<IKernelDriver> make_fpga_driver(
    const DriverFactoryConfig& cfg, Engine& engine) {
    if (!fpga::FpgaDriver::native_fpga()) return nullptr;
    fpga::FpgaConfig fc;
    if (!cfg.fpga_device.empty()) fc.device = cfg.fpga_device;
    auto d = std::make_unique<fpga::FpgaDriver>(std::move(fc), engine);
    if (!d->start()) return nullptr;
    return d;
}

} // namespace hft::core
