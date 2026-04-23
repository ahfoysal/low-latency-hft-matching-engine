#include "feed.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace hft::md {

static_assert(sizeof(sockaddr_in) <= 32, "dest_storage too small");

Feed::Feed(FeedConfig cfg) : cfg_(std::move(cfg)) {}

Feed::~Feed() { close(); }

bool Feed::open() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    auto* dst = reinterpret_cast<sockaddr_in*>(dest_storage_);
    std::memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port   = htons(cfg_.dest_port);
    if (::inet_pton(AF_INET, cfg_.dest_host.c_str(), &dst->sin_addr) != 1) {
        ::close(fd_); fd_ = -1;
        return false;
    }
    if (cfg_.multicast) {
        unsigned char ttl = static_cast<unsigned char>(cfg_.ttl);
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
    return true;
}

void Feed::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

long Feed::publish_l1(std::string_view symbol,
                      double bid_px, double bid_sz,
                      double ask_px, double ask_sz) {
    if (fd_ < 0) return -1;
    ++seq_;
    char buf[256];
    // Cap symbol to avoid format tricks.
    char sym[32];
    std::size_t n = std::min(symbol.size(), sizeof(sym) - 1);
    std::memcpy(sym, symbol.data(), n);
    sym[n] = '\0';
    int len = std::snprintf(buf, sizeof(buf),
                            "L1|%llu|%s|%.2f|%.0f|%.2f|%.0f\n",
                            static_cast<unsigned long long>(seq_),
                            sym, bid_px, bid_sz, ask_px, ask_sz);
    if (len <= 0) return -1;

    auto* dst = reinterpret_cast<sockaddr_in*>(dest_storage_);
    ssize_t sent = ::sendto(fd_, buf, static_cast<std::size_t>(len), 0,
                            reinterpret_cast<sockaddr*>(dst), sizeof(*dst));
    if (sent > 0) ++published_;
    return static_cast<long>(sent);
}

} // namespace hft::md
