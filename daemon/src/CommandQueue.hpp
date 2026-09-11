#pragma once

#include "MDRProtocolV1.hpp"

#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// MDR command queue
//
// The headset handles one DATA frame at a time: it ACKs it, and anything sent
// before that ACK arrives is silently dropped. Commands are therefore queued
// and released one by one. On an ACK timeout the sequence bit is flipped and
// the frame resent, which is how Sony's own client recovers from a lost ACK.
// ---------------------------------------------------------------------------
class CommandQueue {
public:
    using Sender = std::function<void(const std::vector<uint8_t>&)>;
    using Clock = std::chrono::steady_clock;

    explicit CommandQueue(Sender sender, std::chrono::milliseconds ackTimeout = std::chrono::milliseconds(1000))
        : sender_(std::move(sender)), ackTimeout_(ackTimeout) {}

    // Takes a framed packet from the serializers but keeps only its payload:
    // the sequence number is decided when the frame is actually sent.
    //
    // With `coalesce`, a command still waiting in the queue under the same
    // `what` is replaced rather than joined: the newest value wins. That is what
    // stops a dragged slider from queueing a backlog that plays out for seconds
    // after the drag has ended. Never coalesce commands whose every instance
    // matters (queries, raw probes).
    void enqueue(const std::vector<uint8_t>& frame, const char* what, bool coalesce = false) {
        auto unpacked = unpackFrame(frame);
        if (!unpacked) return;
        if (coalesce) {
            for (auto& item : pending_) {
                if (std::string_view(item.what) == what) {
                    item.payload = std::move(unpacked->payload);
                    item.type = unpacked->type;
                    return;
                }
            }
        }
        // Keep the frame type: table-2 commands (DATA_MDR_NO2) share command
        // bytes with table 1, so the type is what tells them apart.
        pending_.push_back({std::move(unpacked->payload), what, unpacked->type});
        pump();
    }

    // Only an ACK says which sequence number the headset expects next. The
    // headset's own DATA frames (notifications, replies) run on a separate
    // count; taking the number from those desynchronises the next command,
    // which then costs a full ACK timeout before the retry lands.
    void onFrame(PacketType type, uint8_t rxSeq) {
        if (type != PacketType::ACK) return;
        seq_ = rxSeq;
        if (inFlight_) {
            inFlight_.reset();
            pump();
        }
    }

    void tick(Clock::time_point now = Clock::now()) {
        if (!inFlight_) return;
        if (now - sentAt_ < ackTimeout_) return;
        if (retries_ >= kMaxRetries) {
            if (log_) fprintf(stderr, "[MDR] No ACK for %s after %d retries; dropping it\n",
                    inFlight_->what, kMaxRetries);
            fflush(stderr);
            inFlight_.reset();
            pump();
            return;
        }
        ++retries_;
        seq_ ^= 1;
        transmit();
    }

    void setLogging(bool enabled) noexcept { log_ = enabled; }

    [[nodiscard]] bool busy() const noexcept { return inFlight_.has_value(); }
    [[nodiscard]] size_t queued() const noexcept { return pending_.size(); }
    [[nodiscard]] uint8_t nextSeq() const noexcept { return seq_; }

    void reset() {
        pending_.clear();
        inFlight_.reset();
        seq_ = 0;
        retries_ = 0;
    }

private:
    struct Item {
        std::vector<uint8_t> payload;
        const char* what;
        PacketType type{PacketType::DATA_MDR};
    };

    static constexpr int kMaxRetries = 3;

    void pump() {
        if (inFlight_ || pending_.empty()) return;
        inFlight_ = std::move(pending_.front());
        pending_.pop_front();
        retries_ = 0;
        transmit();
    }

    void transmit() {
        if (log_) {
            fprintf(stderr, "[MDR] TX%s seq=%u %s:",
                    inFlight_->type == PacketType::DATA_MDR_NO2 ? " T2" : "",
                    static_cast<unsigned>(seq_), inFlight_->what);
            for (uint8_t b : inFlight_->payload) fprintf(stderr, " %02x", b);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        sender_(packFrame(inFlight_->type, seq_, inFlight_->payload));
        sentAt_ = Clock::now();
    }

    Sender sender_;
    std::chrono::milliseconds ackTimeout_;
    std::deque<Item> pending_;
    std::optional<Item> inFlight_;
    Clock::time_point sentAt_;
    uint8_t seq_{0};
    int retries_{0};
    bool log_{false};
};

} // namespace omarchy::sony::protocol
