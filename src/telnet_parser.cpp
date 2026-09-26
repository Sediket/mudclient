#include "mudclient/telnet_parser.hpp"

#include <algorithm>
#include <charconv>

namespace mudclient {

namespace {
constexpr uint8_t IAC = 255;
constexpr uint8_t DONT_CMD = 254;
constexpr uint8_t DO_CMD = 253;
constexpr uint8_t WONT_CMD = 252;
constexpr uint8_t WILL_CMD = 251;
constexpr uint8_t SB_CMD = 250;
constexpr uint8_t GA_CMD = 249;
constexpr uint8_t SE_CMD = 240;
constexpr uint8_t EOR_CMD = 239;

constexpr uint8_t OPT_ECHO [[maybe_unused]] = 1;
constexpr uint8_t OPT_TTYPE = 24;
constexpr uint8_t OPT_EOR = 25;
constexpr uint8_t OPT_NAWS = 31;
constexpr uint8_t OPT_GMCP = 201;

constexpr size_t SB_MAX_SIZE = 64 * 1024;

constexpr uint8_t TTYPE_IS = 0;
constexpr uint8_t TTYPE_SEND = 1;

} // namespace

TelnetParser::TelnetParser(uint16_t naws_width, uint16_t naws_height, std::string terminal_type)
    : naws_width_(naws_width), naws_height_(naws_height), terminal_type_(std::move(terminal_type)) {}

bool TelnetParser::we_agree_to_enable_him(uint8_t opt) {
    // We accept the peer enabling GMCP (so it may send us GMCP messages)
    // and EOR (so IAC EOR marks prompts). Everything else, including MCCP
    // (85/86), is refused.
    return opt == OPT_GMCP || opt == OPT_EOR;
}

bool TelnetParser::we_agree_to_enable_us(uint8_t opt) {
    // We agree to enable NAWS/TTYPE on our side when the peer asks (DO),
    // since we can answer both with fixed, static values.
    return opt == OPT_NAWS || opt == OPT_TTYPE;
}

void TelnetParser::feed(std::span<const uint8_t> bytes, std::vector<OutputEvent>& out_events,
                         std::string& out_to_send) {
    events_ = &out_events;
    send_ = &out_to_send;
    for (uint8_t b : bytes) {
        process_byte(b);
    }
    events_ = nullptr;
    send_ = nullptr;
}

bool TelnetParser::flush_idle(std::vector<OutputEvent>& out_events) {
    if (current_plain_.empty()) {
        return false;
    }
    events_ = &out_events;
    complete_line(/*is_prompt=*/true);
    events_ = nullptr;
    return true;
}

void TelnetParser::process_byte(uint8_t b) {
    switch (state_) {
    case State::Data:
        process_data_byte(b);
        break;
    case State::Iac:
        process_iac_command(b);
        break;
    case State::Will:
        handle_will(b);
        state_ = State::Data;
        break;
    case State::Wont:
        handle_wont(b);
        state_ = State::Data;
        break;
    case State::Do:
        handle_do(b);
        state_ = State::Data;
        break;
    case State::Dont:
        handle_dont(b);
        state_ = State::Data;
        break;
    case State::Sb:
        sb_option_ = b;
        sb_buffer_.clear();
        sb_overflowed_ = false;
        state_ = State::SbData;
        break;
    case State::SbData:
        if (b == IAC) {
            state_ = State::SbDataIac;
        } else {
            append_sb_byte(b);
        }
        break;
    case State::SbDataIac:
        if (b == IAC) {
            append_sb_byte(0xFF);
            state_ = State::SbData;
        } else if (b == SE_CMD) {
            complete_subnegotiation();
            state_ = State::Data;
        } else {
            // Malformed stream: IAC <cmd> appeared inside a subnegotiation
            // without a preceding SE. Abandon the subnegotiation (discarded,
            // not dispatched) and reprocess this byte as a fresh IAC command
            // so we don't lose synchronization with the rest of the stream.
            sb_buffer_.clear();
            state_ = State::Iac;
            process_iac_command(b);
        }
        break;
    case State::Esc:
        if (b == '[') {
            csi_params_.clear();
            state_ = State::Csi;
        } else {
            // Unsupported/unknown escape: drop the ESC and reprocess this
            // byte as ordinary data.
            state_ = State::Data;
            process_data_byte(b);
        }
        break;
    case State::Csi:
        if (b >= 0x30 && b <= 0x3F) {
            csi_params_.push_back(b);
        } else if (b >= 0x20 && b <= 0x2F) {
            // Intermediate bytes: not used by SGR, consumed silently.
        } else if (b >= 0x40 && b <= 0x7E) {
            if (b == 'm') {
                std::vector<int> codes;
                if (csi_params_.empty()) {
                    codes.push_back(0);
                } else {
                    size_t start = 0;
                    for (size_t i = 0; i <= csi_params_.size(); ++i) {
                        if (i == csi_params_.size() || csi_params_[i] == ';') {
                            int value = 0;
                            if (i > start) {
                                std::from_chars(reinterpret_cast<const char*>(csi_params_.data() + start),
                                                 reinterpret_cast<const char*>(csi_params_.data() + i), value);
                            }
                            codes.push_back(value);
                            start = i + 1;
                        }
                    }
                }
                apply_sgr(codes);
            }
            // Any other CSI final byte: sequence is silently consumed.
            state_ = State::Data;
        } else {
            // Invalid byte inside a CSI sequence: bail out to Data.
            state_ = State::Data;
        }
        break;
    }
}

void TelnetParser::process_data_byte(uint8_t b) {
    if (b == IAC) {
        state_ = State::Iac;
    } else if (b == 0x1B) {
        state_ = State::Esc;
    } else if (b == '\n') {
        complete_line(/*is_prompt=*/false);
    } else if (b == '\r') {
        // Dropped: real line breaks are signalled by '\n' (or IAC GA/EOR);
        // a bare CR carries no information for us.
    } else {
        append_char(static_cast<char>(b));
    }
}

void TelnetParser::process_iac_command(uint8_t b) {
    switch (b) {
    case IAC:
        append_char(static_cast<char>(0xFF));
        state_ = State::Data;
        break;
    case WILL_CMD:
        state_ = State::Will;
        break;
    case WONT_CMD:
        state_ = State::Wont;
        break;
    case DO_CMD:
        state_ = State::Do;
        break;
    case DONT_CMD:
        state_ = State::Dont;
        break;
    case SB_CMD:
        state_ = State::Sb;
        break;
    case GA_CMD:
    case EOR_CMD:
        complete_line(/*is_prompt=*/true);
        state_ = State::Data;
        break;
    default:
        // Other telnet commands (NOP, DM, AYT, ...) carry no payload and
        // require no reply; simply return to Data.
        state_ = State::Data;
        break;
    }
}

// ---- RFC 1143 Q-method negotiation ----
//
// Each option has two independent sides: "us" (do we have it enabled) and
// "him" (does the peer have it enabled). Each side has a state in
// {No, Yes, WantNo, WantYes} and, while in WantNo/WantYes, a queued flag
// meaning "as soon as this settles, immediately request the opposite".
// This prevents negotiation loops when both ends propose a change to the
// same option around the same time. Limitation: this implementation only
// reacts to peer-initiated negotiation (we never spontaneously start a
// WantYes/WantNo ourselves outside of the reactive paths below), so the
// "queued opposite" branches exist for protocol correctness/robustness but
// are not exercised by our own negotiation traffic in this milestone.

void TelnetParser::handle_will(uint8_t opt) {
    OptionSide& him = options_[opt].him;
    switch (him.state) {
    case OptState::No:
        if (we_agree_to_enable_him(opt)) {
            him.state = OptState::Yes;
            send_iac(DO_CMD, opt);
            on_him_became_yes(opt);
        } else {
            send_iac(DONT_CMD, opt);
        }
        break;
    case OptState::Yes:
        break; // already enabled; ignore duplicate
    case OptState::WantNo:
        if (!him.queued_opposite) {
            him.state = OptState::No; // answered contrary to our DONT; treat as error, stay disabled
        } else {
            him.state = OptState::Yes;
            him.queued_opposite = false;
        }
        break;
    case OptState::WantYes:
        if (!him.queued_opposite) {
            him.state = OptState::Yes;
            on_him_became_yes(opt);
        } else {
            him.state = OptState::WantNo;
            him.queued_opposite = false;
            send_iac(DONT_CMD, opt);
        }
        break;
    }
}

void TelnetParser::handle_wont(uint8_t opt) {
    OptionSide& him = options_[opt].him;
    switch (him.state) {
    case OptState::No:
        break; // already disabled; ignore
    case OptState::Yes:
        him.state = OptState::No;
        send_iac(DONT_CMD, opt);
        break;
    case OptState::WantNo:
        if (!him.queued_opposite) {
            him.state = OptState::No;
        } else {
            him.state = OptState::WantYes;
            him.queued_opposite = false;
            send_iac(DO_CMD, opt);
        }
        break;
    case OptState::WantYes:
        him.state = OptState::No;
        him.queued_opposite = false;
        break;
    }
}

void TelnetParser::handle_do(uint8_t opt) {
    OptionSide& us = options_[opt].us;
    switch (us.state) {
    case OptState::No:
        if (we_agree_to_enable_us(opt)) {
            us.state = OptState::Yes;
            send_iac(WILL_CMD, opt);
            on_us_became_yes(opt);
        } else {
            send_iac(WONT_CMD, opt);
        }
        break;
    case OptState::Yes:
        break; // already enabled; ignore duplicate
    case OptState::WantNo:
        if (!us.queued_opposite) {
            us.state = OptState::No;
        } else {
            us.state = OptState::Yes;
            us.queued_opposite = false;
        }
        break;
    case OptState::WantYes:
        if (!us.queued_opposite) {
            us.state = OptState::Yes;
            on_us_became_yes(opt);
        } else {
            us.state = OptState::WantNo;
            us.queued_opposite = false;
            send_iac(WONT_CMD, opt);
        }
        break;
    }
}

void TelnetParser::handle_dont(uint8_t opt) {
    OptionSide& us = options_[opt].us;
    switch (us.state) {
    case OptState::No:
        break;
    case OptState::Yes:
        us.state = OptState::No;
        send_iac(WONT_CMD, opt);
        break;
    case OptState::WantNo:
        if (!us.queued_opposite) {
            us.state = OptState::No;
        } else {
            us.state = OptState::WantYes;
            us.queued_opposite = false;
            send_iac(WILL_CMD, opt);
        }
        break;
    case OptState::WantYes:
        us.state = OptState::No;
        us.queued_opposite = false;
        break;
    }
}

void TelnetParser::on_him_became_yes(uint8_t opt) {
    if (opt == OPT_GMCP) {
        send_subnegotiation(OPT_GMCP, std::string_view("Core.Hello {\"client\":\"mudclient\",\"version\":\"0.1.0\"}"));
        send_subnegotiation(OPT_GMCP, std::string_view("Core.Supports.Set [\"Core 1\"]"));
    }
}

void TelnetParser::on_us_became_yes(uint8_t opt) {
    if (opt == OPT_NAWS) {
        uint8_t payload[4] = {
            static_cast<uint8_t>(naws_width_ >> 8),
            static_cast<uint8_t>(naws_width_ & 0xFF),
            static_cast<uint8_t>(naws_height_ >> 8),
            static_cast<uint8_t>(naws_height_ & 0xFF),
        };
        send_subnegotiation(OPT_NAWS, std::span<const uint8_t>(payload, 4));
    }
    // TTYPE: no proactive subnegotiation; we answer when the peer sends
    // SB TTYPE SEND (see complete_subnegotiation()).
}

void TelnetParser::send_iac(uint8_t cmd, uint8_t opt) {
    if (!send_) return;
    send_->push_back(static_cast<char>(IAC));
    send_->push_back(static_cast<char>(cmd));
    send_->push_back(static_cast<char>(opt));
}

void TelnetParser::send_subnegotiation(uint8_t opt, std::span<const uint8_t> payload) {
    if (!send_) return;
    send_->push_back(static_cast<char>(IAC));
    send_->push_back(static_cast<char>(SB_CMD));
    send_->push_back(static_cast<char>(opt));
    for (uint8_t byte : payload) {
        if (byte == IAC) {
            send_->push_back(static_cast<char>(IAC));
        }
        send_->push_back(static_cast<char>(byte));
    }
    send_->push_back(static_cast<char>(IAC));
    send_->push_back(static_cast<char>(SE_CMD));
}

void TelnetParser::append_sb_byte(uint8_t b) {
    if (sb_overflowed_) {
        return;
    }
    if (sb_buffer_.size() >= SB_MAX_SIZE) {
        sb_overflowed_ = true;
        ++overflow_count_;
        sb_buffer_.clear();
        return;
    }
    sb_buffer_.push_back(b);
}

void TelnetParser::complete_subnegotiation() {
    if (sb_overflowed_) {
        sb_overflowed_ = false;
        return; // discard: capped and logged via overflow_count_
    }
    if (sb_option_ == OPT_GMCP) {
        std::string_view text(reinterpret_cast<const char*>(sb_buffer_.data()), sb_buffer_.size());
        size_t space = text.find(' ');
        std::string package(space == std::string_view::npos ? text : text.substr(0, space));
        nlohmann::json json = nlohmann::json::object();
        if (space != std::string_view::npos) {
            std::string_view json_text = text.substr(space + 1);
            json = nlohmann::json::parse(json_text, nullptr, false);
        }
        if (events_) {
            events_->push_back(GmcpMessage{std::move(package), std::move(json)});
        }
    } else if (sb_option_ == OPT_TTYPE) {
        if (!sb_buffer_.empty() && sb_buffer_[0] == TTYPE_SEND) {
            std::vector<uint8_t> reply;
            reply.push_back(TTYPE_IS);
            for (char c : terminal_type_) {
                reply.push_back(static_cast<uint8_t>(c));
            }
            send_subnegotiation(OPT_TTYPE, std::span<const uint8_t>(reply.data(), reply.size()));
        }
    }
    // Other option subnegotiations are accepted but ignored.
}

void TelnetParser::append_char(char c) {
    current_plain_.push_back(c);
}

void TelnetParser::set_style(TextStyle new_style) {
    if (new_style == pending_style_) {
        return;
    }
    uint32_t len = static_cast<uint32_t>(current_plain_.size()) - pending_span_start_;
    if (len > 0) {
        current_spans_.push_back(StyledSpan{pending_style_, pending_span_start_, len});
    }
    pending_style_ = new_style;
    pending_span_start_ = static_cast<uint32_t>(current_plain_.size());
}

void TelnetParser::complete_line(bool is_prompt) {
    // Close the currently open span before handing off the line.
    uint32_t len = static_cast<uint32_t>(current_plain_.size()) - pending_span_start_;
    std::vector<StyledSpan> spans = std::move(current_spans_);
    if (len > 0) {
        spans.push_back(StyledSpan{pending_style_, pending_span_start_, len});
    }

    StyledLine line;
    line.plain = std::move(current_plain_);
    line.spans = std::move(spans);
    line.is_prompt = is_prompt;

    current_plain_.clear();
    current_spans_.clear();
    // Style persists across lines: pending_style_ is intentionally left
    // untouched; only the span start resets to the new line's origin.
    pending_span_start_ = 0;

    if (events_) {
        events_->push_back(std::move(line));
    }
}

void TelnetParser::apply_sgr(const std::vector<int>& codes) {
    TextStyle style = pending_style_;
    for (size_t i = 0; i < codes.size(); ++i) {
        int code = codes[i];
        if (code == 0) {
            style = TextStyle{};
        } else if (code == 1) {
            style.flags |= style_bold;
        } else if (code == 3) {
            style.flags |= style_italic;
        } else if (code == 4) {
            style.flags |= style_underline;
        } else if (code == 5) {
            style.flags |= style_blink;
        } else if (code == 7) {
            style.flags |= style_inverse;
        } else if (code == 22) {
            style.flags &= static_cast<uint8_t>(~style_bold);
        } else if (code == 23) {
            style.flags &= static_cast<uint8_t>(~style_italic);
        } else if (code == 24) {
            style.flags &= static_cast<uint8_t>(~style_underline);
        } else if (code == 25) {
            style.flags &= static_cast<uint8_t>(~style_blink);
        } else if (code == 27) {
            style.flags &= static_cast<uint8_t>(~style_inverse);
        } else if (code >= 30 && code <= 37) {
            style.fg = encode_standard_color(static_cast<uint8_t>(code - 30));
        } else if (code == 38) {
            if (i + 2 < codes.size() && codes[i + 1] == 5) {
                style.fg = encode_256_color(static_cast<uint8_t>(codes[i + 2]));
                i += 2;
            } else if (i + 4 < codes.size() && codes[i + 1] == 2) {
                style.fg = encode_truecolor(static_cast<uint8_t>(codes[i + 2]), static_cast<uint8_t>(codes[i + 3]),
                                             static_cast<uint8_t>(codes[i + 4]));
                i += 4;
            } else {
                // Truncated extended-color sequence (missing index/RGB
                // components): abandon the rest of this SGR sequence rather
                // than reinterpreting the leftover sub-parameters (e.g. the
                // "5" in a bare "38;5") as independent top-level codes.
                break;
            }
        } else if (code == 39) {
            style.fg = default_fg;
        } else if (code >= 40 && code <= 47) {
            style.bg = encode_standard_color(static_cast<uint8_t>(code - 40));
        } else if (code == 48) {
            if (i + 2 < codes.size() && codes[i + 1] == 5) {
                style.bg = encode_256_color(static_cast<uint8_t>(codes[i + 2]));
                i += 2;
            } else if (i + 4 < codes.size() && codes[i + 1] == 2) {
                style.bg = encode_truecolor(static_cast<uint8_t>(codes[i + 2]), static_cast<uint8_t>(codes[i + 3]),
                                             static_cast<uint8_t>(codes[i + 4]));
                i += 4;
            } else {
                break; // truncated extended-color sequence; see the 38 case above
            }
        } else if (code == 49) {
            style.bg = default_bg;
        } else if (code >= 90 && code <= 97) {
            style.fg = encode_standard_color(static_cast<uint8_t>(code - 90 + 8));
        } else if (code >= 100 && code <= 107) {
            style.bg = encode_standard_color(static_cast<uint8_t>(code - 100 + 8));
        }
        // Other codes (2, 6, 8, 9, 26, 28, 29, ...): silently ignored, no
        // corresponding flag is tracked.
    }
    set_style(style);
}

} // namespace mudclient
