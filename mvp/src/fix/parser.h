#pragma once

// Minimal FIX 4.4 tag=value parser.
//
// FIX messages are ASCII key=value pairs separated by SOH (0x01):
//   8=FIX.4.4<SOH>9=<bodylen><SOH>35=<type><SOH>...<SOH>10=<checksum><SOH>
//
// We hand-roll this rather than pulling QuickFIX because (a) this project
// is about measuring low-latency paths end-to-end, and (b) the subset we
// actually speak (A, 5, 0, D, F, V, W, 8) is tiny.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hft::fix {

inline constexpr char SOH = '\x01';

// Common tag numbers we care about.
enum Tag : int {
    BeginString     = 8,
    BodyLength      = 9,
    MsgType         = 35,
    SenderCompID    = 49,
    TargetCompID    = 56,
    MsgSeqNum       = 34,
    SendingTime     = 52,
    CheckSum        = 10,

    ClOrdID         = 11,
    OrigClOrdID     = 41,
    OrderID         = 37,
    ExecID          = 17,
    ExecType        = 150,
    OrdStatus       = 39,
    Symbol          = 55,
    SideTag         = 54,
    OrderQty        = 38,
    CumQty          = 14,
    LeavesQty       = 151,
    LastQty         = 32,
    LastPx          = 31,
    PriceTag        = 44,
    AvgPx           = 6,
    OrdType         = 40,
    TimeInForce     = 59,
    TransactTime    = 60,

    MDReqID         = 262,
    SubscriptionRequestType = 263,
    MarketDepth     = 264,
    NoRelatedSym    = 146,
    NoMDEntries     = 268,
    MDEntryType     = 269,
    MDEntryPx       = 270,
    MDEntrySize     = 271,

    EncryptMethod   = 98,
    HeartBtInt      = 108,
    TestReqID       = 112,
    Text            = 58,
};

// One (tag, value) pair referencing bytes inside the source buffer.
// Views are only valid for the lifetime of that buffer.
struct Field {
    int              tag;
    std::string_view value;
};

// Parse result for split_frame().
enum class FrameStatus {
    Ok,
    Incomplete,     // need more bytes
    Malformed,      // garbage; caller should drop the session
};

// Parsed message: a flat list of fields in wire order. Small-vector avoidance
// isn't worth it here — messages we handle have <30 tags.
struct Message {
    std::vector<Field> fields;

    // Convenience lookups. Returns empty view if not present.
    std::string_view get(int tag) const noexcept;
    bool             has(int tag) const noexcept;
    long long        get_int(int tag, long long def = 0) const noexcept;
    double           get_double(int tag, double def = 0.0) const noexcept;
    char             msg_type() const noexcept;  // first byte of tag 35, or 0
};

// Try to extract one complete FIX frame from `buf` (a streaming TCP buffer).
// On success: writes the frame bytes to `*out_frame` and consumed length to
// `*out_consumed`. On Incomplete: consumed=0. On Malformed: caller should
// disconnect.
FrameStatus split_frame(std::string_view buf,
                        std::string_view* out_frame,
                        std::size_t*      out_consumed) noexcept;

// Parse a *complete* frame (as returned by split_frame) into fields.
// Does not re-validate the checksum beyond what split_frame already did.
bool parse_frame(std::string_view frame, Message* out) noexcept;

// Compute the FIX checksum: sum of all bytes up to (but not including) the
// "10=..." field, mod 256, rendered as 3-digit zero-padded decimal.
uint8_t checksum(std::string_view bytes) noexcept;

// -------- Builder helpers (used by the gateway to emit ExecReports etc.) ----

class Builder {
public:
    Builder();

    // Start a new message. Clears prior state and emits 8=FIX.4.4 header.
    // body fields are appended via add_*, then finalize() writes 9= and 10=.
    void begin(char msg_type,
               std::string_view sender_comp,
               std::string_view target_comp,
               uint32_t         seq_num);

    void add_str(int tag, std::string_view v);
    void add_int(int tag, long long v);
    void add_double(int tag, double v, int decimals = 2);
    void add_char(int tag, char c);

    // Returns the complete framed message (includes trailer).
    // The returned view is valid until the next begin()/finalize().
    std::string_view finalize();

    const std::string& buffer() const { return buf_; }

private:
    std::string buf_;
    std::size_t body_start_ = 0;  // index of first byte after "9=xxxxxSOH"
    std::size_t body_len_slot_ = 0;  // where we'll back-patch bodylen digits
};

} // namespace hft::fix
