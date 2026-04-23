#include "parser.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace hft::fix {

// -------- Message accessors --------------------------------------------------

std::string_view Message::get(int tag) const noexcept {
    for (auto const& f : fields) {
        if (f.tag == tag) return f.value;
    }
    return {};
}

bool Message::has(int tag) const noexcept {
    for (auto const& f : fields) {
        if (f.tag == tag) return true;
    }
    return false;
}

long long Message::get_int(int tag, long long def) const noexcept {
    auto v = get(tag);
    if (v.empty()) return def;
    long long out = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{}) return def;
    return out;
}

double Message::get_double(int tag, double def) const noexcept {
    auto v = get(tag);
    if (v.empty()) return def;
    // std::from_chars<double> is patchy in libc++ on some macOS SDKs, so we
    // fall through to strtod on a null-terminated stack copy for robustness.
    char tmp[64];
    if (v.size() >= sizeof(tmp)) return def;
    std::memcpy(tmp, v.data(), v.size());
    tmp[v.size()] = '\0';
    char* end = nullptr;
    double d = std::strtod(tmp, &end);
    if (end == tmp) return def;
    return d;
}

char Message::msg_type() const noexcept {
    auto v = get(MsgType);
    return v.empty() ? 0 : v[0];
}

// -------- Framing ------------------------------------------------------------

uint8_t checksum(std::string_view bytes) noexcept {
    uint32_t sum = 0;
    for (unsigned char c : bytes) sum += c;
    return static_cast<uint8_t>(sum & 0xFF);
}

// We accept a small preamble flexibility: the first field must be 8=FIX.4.4SOH
// and the second 9=<digits>SOH. After that we read exactly <bodylen> bytes of
// body, then expect "10=NNNSOH".
FrameStatus split_frame(std::string_view buf,
                        std::string_view* out_frame,
                        std::size_t*      out_consumed) noexcept {
    *out_consumed = 0;
    static constexpr std::string_view kBegin = "8=FIX.4.4\x01";

    if (buf.size() < kBegin.size()) return FrameStatus::Incomplete;
    if (buf.substr(0, kBegin.size()) != kBegin) return FrameStatus::Malformed;

    // Expect "9=" right after.
    std::size_t pos = kBegin.size();
    if (buf.size() < pos + 2) return FrameStatus::Incomplete;
    if (buf[pos] != '9' || buf[pos + 1] != '=') return FrameStatus::Malformed;
    pos += 2;

    // Read body length digits until SOH.
    std::size_t len_start = pos;
    while (pos < buf.size() && buf[pos] != SOH) {
        if (buf[pos] < '0' || buf[pos] > '9') return FrameStatus::Malformed;
        ++pos;
    }
    if (pos >= buf.size()) return FrameStatus::Incomplete;
    std::size_t body_len = 0;
    auto [p, ec] = std::from_chars(buf.data() + len_start,
                                   buf.data() + pos, body_len);
    if (ec != std::errc{}) return FrameStatus::Malformed;
    if (body_len > 1 << 20) return FrameStatus::Malformed;  // 1MB hard cap
    ++pos;  // skip SOH after bodylen

    std::size_t body_end = pos + body_len;
    // Trailer is "10=NNN" + SOH = 7 bytes.
    std::size_t total = body_end + 7;
    if (buf.size() < total) return FrameStatus::Incomplete;

    if (buf[body_end] != '1' || buf[body_end + 1] != '0' ||
        buf[body_end + 2] != '=' || buf[total - 1] != SOH) {
        return FrameStatus::Malformed;
    }

    // Validate checksum: sum over everything up to (not including) the "10="
    uint8_t expected = checksum(buf.substr(0, body_end));
    uint8_t got = 0;
    for (std::size_t i = body_end + 3; i < total - 1; ++i) {
        if (buf[i] < '0' || buf[i] > '9') return FrameStatus::Malformed;
        got = static_cast<uint8_t>(got * 10 + (buf[i] - '0'));
    }
    if (got != expected) return FrameStatus::Malformed;

    *out_frame    = buf.substr(0, total);
    *out_consumed = total;
    return FrameStatus::Ok;
}

bool parse_frame(std::string_view frame, Message* out) noexcept {
    out->fields.clear();
    out->fields.reserve(24);

    std::size_t i = 0;
    while (i < frame.size()) {
        // tag digits
        std::size_t t0 = i;
        while (i < frame.size() && frame[i] != '=') {
            if (frame[i] < '0' || frame[i] > '9') return false;
            ++i;
        }
        if (i >= frame.size()) return false;
        int tag = 0;
        auto [p, ec] = std::from_chars(frame.data() + t0,
                                       frame.data() + i, tag);
        if (ec != std::errc{}) return false;
        ++i;  // skip '='

        std::size_t v0 = i;
        while (i < frame.size() && frame[i] != SOH) ++i;
        if (i >= frame.size()) return false;
        out->fields.push_back(Field{tag, frame.substr(v0, i - v0)});
        ++i;  // skip SOH
    }
    return true;
}

// -------- Builder ------------------------------------------------------------

Builder::Builder() { buf_.reserve(256); }

void Builder::begin(char msg_type,
                    std::string_view sender_comp,
                    std::string_view target_comp,
                    uint32_t seq_num) {
    buf_.clear();
    // Header.
    buf_ += "8=FIX.4.4";
    buf_ += SOH;
    // Reserve 6 digits for body length; we back-patch on finalize().
    buf_ += "9=";
    body_len_slot_ = buf_.size();
    buf_ += "000000";
    buf_ += SOH;
    body_start_ = buf_.size();

    add_char(MsgType, msg_type);
    add_str(SenderCompID, sender_comp);
    add_str(TargetCompID, target_comp);
    add_int(MsgSeqNum, seq_num);
}

void Builder::add_str(int tag, std::string_view v) {
    char numbuf[16];
    auto [p, ec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), tag);
    (void)ec;
    buf_.append(numbuf, p);
    buf_ += '=';
    buf_ += v;
    buf_ += SOH;
}

void Builder::add_int(int tag, long long v) {
    char numbuf[32];
    auto [tp, tec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), tag);
    (void)tec;
    buf_.append(numbuf, tp);
    buf_ += '=';
    char vbuf[32];
    auto [vp, vec] = std::to_chars(vbuf, vbuf + sizeof(vbuf), v);
    (void)vec;
    buf_.append(vbuf, vp);
    buf_ += SOH;
}

void Builder::add_double(int tag, double v, int decimals) {
    char numbuf[16];
    auto [p, ec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), tag);
    (void)ec;
    buf_.append(numbuf, p);
    buf_ += '=';
    char vbuf[64];
    int n = std::snprintf(vbuf, sizeof(vbuf), "%.*f", decimals, v);
    if (n > 0) buf_.append(vbuf, static_cast<std::size_t>(n));
    buf_ += SOH;
}

void Builder::add_char(int tag, char c) {
    char numbuf[16];
    auto [p, ec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), tag);
    (void)ec;
    buf_.append(numbuf, p);
    buf_ += '=';
    buf_ += c;
    buf_ += SOH;
}

std::string_view Builder::finalize() {
    // Patch body length.
    std::size_t body_len = buf_.size() - body_start_;
    char lenbuf[7];
    std::snprintf(lenbuf, sizeof(lenbuf), "%06zu", body_len);
    std::memcpy(&buf_[body_len_slot_], lenbuf, 6);

    // Checksum over everything so far.
    uint8_t cs = checksum(buf_);
    char csbuf[16];
    int n = std::snprintf(csbuf, sizeof(csbuf), "10=%03u%c", cs, SOH);
    buf_.append(csbuf, static_cast<std::size_t>(n));
    return std::string_view{buf_};
}

} // namespace hft::fix
