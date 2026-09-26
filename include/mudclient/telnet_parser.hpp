#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "mudclient/events.hpp"

// Streaming, byte-at-a-time telnet + ANSI parser. Runs on the network
// thread only; never touches Lua or any engine-thread state.
//
// The parser is fully resumable across arbitrary chunk boundaries: every
// byte is processed exactly once, through a state machine whose state lives
// entirely in member variables (never on the call stack), so splitting a
// feed() call into any number of smaller feed() calls at any byte offset
// produces identical output. This holds for IAC/SB/SE sequences, IAC IAC
// escapes (in data and in subnegotiation payloads), ANSI CSI sequences, and
// UTF-8 text: UTF-8 continuation bytes (0x80-0xBF) never match any
// recognized control byte (IAC, ESC, CSI parameter/final bytes are all
// <0x80), so they always fall through to plain data accumulation regardless
// of where a chunk boundary lands.
namespace mudclient {

struct GmcpMessage {
    std::string package;
    nlohmann::json json;

    friend bool operator==(const GmcpMessage& a, const GmcpMessage& b) {
        // nlohmann::json treats two "discarded" (failed-parse) values as
        // never equal to anything, including each other; for our purposes
        // two discarded payloads for the same package are equal.
        if (a.json.is_discarded() && b.json.is_discarded()) {
            return a.package == b.package;
        }
        return a.package == b.package && a.json == b.json;
    }
};

class TelnetParser {
public:
    using OutputEvent = std::variant<StyledLine, GmcpMessage>;

    explicit TelnetParser(uint16_t naws_width = 80, uint16_t naws_height = 24,
                          std::string terminal_type = "MUDCLIENT");

    // Feed raw bytes read from the socket. Any complete lines or GMCP
    // messages are appended to `out_events`; any telnet negotiation replies
    // (WILL/WONT/DO/DONT, subnegotiation responses) are appended as raw
    // bytes to `out_to_send`, which the caller is responsible for writing to
    // the socket, in order, after the bytes already queued.
    void feed(std::span<const uint8_t> bytes, std::vector<OutputEvent>& out_events,
              std::string& out_to_send);

    // Call when no new bytes have arrived for the idle threshold (250ms per
    // spec) and there is a non-empty partial line buffered. Appends a
    // prompt-marked line built from the partial buffer to `out_events` and
    // clears it. Returns false (no-op) if there is nothing buffered.
    bool flush_idle(std::vector<OutputEvent>& out_events);

    bool has_pending_partial_line() const { return !current_plain_.empty(); }

    // Number of subnegotiation payloads discarded for exceeding the 64KB cap.
    uint64_t overflow_count() const { return overflow_count_; }

private:
    enum class State {
        Data,
        Iac,
        Will,
        Wont,
        Do,
        Dont,
        Sb,
        SbData,
        SbDataIac,
        Esc,
        Csi,
    };

    enum class OptState : uint8_t { No, Yes, WantNo, WantYes };

    struct OptionSide {
        OptState state = OptState::No;
        bool queued_opposite = false;
    };

    struct OptionEntry {
        OptionSide us;  // do we have the option enabled
        OptionSide him; // does the peer have the option enabled
    };

    // Per-byte state transition. May append to events_/send_ (set for the
    // duration of the enclosing feed() call).
    void process_byte(uint8_t b);
    void process_data_byte(uint8_t b);
    void process_iac_command(uint8_t b);

    // RFC 1143 Q-method option negotiation. `agree` decides whether we are
    // willing to enable the option on the given side when first asked.
    void handle_will(uint8_t opt);
    void handle_wont(uint8_t opt);
    void handle_do(uint8_t opt);
    void handle_dont(uint8_t opt);

    static bool we_agree_to_enable_him(uint8_t opt); // WILL from peer -> DO
    static bool we_agree_to_enable_us(uint8_t opt);  // DO from peer -> WILL

    void on_him_became_yes(uint8_t opt);
    void on_us_became_yes(uint8_t opt);

    void send_iac(uint8_t cmd, uint8_t opt);
    void send_subnegotiation(uint8_t opt, std::span<const uint8_t> payload);
    void send_subnegotiation(uint8_t opt, std::string_view payload) {
        send_subnegotiation(opt, std::span<const uint8_t>(
                                      reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
    }

    void append_sb_byte(uint8_t b);
    void complete_subnegotiation();

    void append_char(char c);
    void complete_line(bool is_prompt);
    void set_style(TextStyle new_style);

    void apply_sgr(const std::vector<int>& codes);

    // ---- Telnet FSM state ----
    State state_ = State::Data;
    std::array<OptionEntry, 256> options_{};

    uint8_t sb_option_ = 0;
    std::vector<uint8_t> sb_buffer_;
    bool sb_overflowed_ = false;
    uint64_t overflow_count_ = 0;

    // ---- ANSI/CSI state ----
    std::vector<uint8_t> csi_params_;

    // ---- Line accumulation state (persists across feed() calls) ----
    std::string current_plain_;
    std::vector<StyledSpan> current_spans_;
    TextStyle pending_style_{};
    uint32_t pending_span_start_ = 0;

    // ---- Local configuration for reactive negotiation replies ----
    uint16_t naws_width_;
    uint16_t naws_height_;
    std::string terminal_type_;

    // ---- Scratch pointers valid only during feed()/flush_idle() ----
    std::vector<OutputEvent>* events_ = nullptr;
    std::string* send_ = nullptr;
};

} // namespace mudclient
