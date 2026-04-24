// M6: mock PTP oracle.
//
// Stand-alone process that answers the PtpClient mock sync exchange.
// Run it on the "master" host (or loopback for testing):
//
//   ./ptp_oracle 0.0.0.0 9881
//
// Wire format (text, one datagram per request):
//   request : "PTP1 T1=<ns>\n"
//   reply   : "PTP1 T1=<ns> T2=<ns> T3=<ns>\n"
//
// T2 is the master's recv timestamp, T3 is its send timestamp; the client
// already has T1/T4 locally and computes offset/delay from the four.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int64_t mono_ns() {
    auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

} // namespace

int main(int argc, char** argv) {
    const char* bind_host = (argc > 1) ? argv[1] : "0.0.0.0";
    uint16_t    bind_port = (argc > 2) ? (uint16_t)std::atoi(argv[2]) : 9881;

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port   = htons(bind_port);
    if (::inet_pton(AF_INET, bind_host, &me.sin_addr) != 1) {
        std::fprintf(stderr, "bad bind host: %s\n", bind_host);
        return 1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) {
        std::perror("bind"); return 1;
    }

    std::printf("ptp_oracle listening on %s:%u\n", bind_host, bind_port);
    char buf[256];
    for (;;) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<sockaddr*>(&peer), &plen);
        int64_t t2 = mono_ns();
        if (n <= 0) continue;
        buf[n] = '\0';

        long long t1 = 0;
        if (std::sscanf(buf, "PTP1 T1=%lld", &t1) != 1) continue;

        // Simulated master-clock offset. In a real deployment this is the
        // GPS-disciplined grandmaster's nanosecond. Here we pick a stable
        // offset so tests see "clock jumps" they can assert on.
        const int64_t kMasterOffsetNs = 1'234'567;
        int64_t t3 = mono_ns() + kMasterOffsetNs;
        t2 += kMasterOffsetNs;

        char out[256];
        int olen = std::snprintf(out, sizeof(out),
                                 "PTP1 T1=%lld T2=%lld T3=%lld\n",
                                 t1, (long long)t2, (long long)t3);
        if (olen > 0) {
            ::sendto(fd, out, (size_t)olen, 0,
                     reinterpret_cast<sockaddr*>(&peer), plen);
        }
    }
}
