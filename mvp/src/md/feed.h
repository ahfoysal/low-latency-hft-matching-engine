#pragma once

// UDP market data feed publisher.
//
// On every book change the gateway calls publish_l1() with a top-of-book
// snapshot. We serialize it to a compact ASCII line (easy to tcpdump, easy
// to eyeball) and blast it as a single UDP datagram. By default we send to
// a loopback unicast address; configure a multicast group to fan out to
// many consumers without extra per-subscriber cost on the sender.

#include <cstdint>
#include <string>
#include <string_view>

namespace hft::md {

struct FeedConfig {
    std::string dest_host   = "127.0.0.1";
    uint16_t    dest_port   = 9879;
    bool        multicast   = false;      // set true for 239.x.x.x addresses
    int         ttl         = 1;          // multicast TTL
};

// Minimal L1 snapshot wire record (ASCII):
//   "L1|<seq>|<symbol>|<bid_px>|<bid_sz>|<ask_px>|<ask_sz>\n"
// kept human-readable on purpose — M3 is about the plumbing. M4 will swap
// to a packed binary struct when we care about bytes-on-wire.
class Feed {
public:
    explicit Feed(FeedConfig cfg);
    ~Feed();

    Feed(const Feed&)            = delete;
    Feed& operator=(const Feed&) = delete;

    // Opens the UDP socket. Returns false on failure.
    bool open();
    void close();

    // Publish a top-of-book snapshot. Sequence numbers auto-increment so
    // consumers can detect drops. Returns bytes sent (or -1 on error).
    long publish_l1(std::string_view symbol,
                    double bid_px, double bid_sz,
                    double ask_px, double ask_sz);

    uint64_t seq() const { return seq_; }
    uint64_t published() const { return published_; }

private:
    FeedConfig cfg_;
    int        fd_{-1};
    // sockaddr_in stored as raw bytes to avoid leaking <netinet/in.h> here.
    alignas(16) unsigned char dest_storage_[32]{};
    uint64_t   seq_{0};
    uint64_t   published_{0};
};

} // namespace hft::md
