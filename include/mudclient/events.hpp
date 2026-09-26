#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

// Event and text-styling types shared between the network thread (producer)
// and the engine thread (consumer). Instances are moved across the
// network->engine MPSC queue (see event_queue.hpp); after a value is pushed,
// only the engine thread touches it.
namespace mudclient {

// Sentinel meaning "use the terminal's default color" for both fg and bg.
// Distinguished from real colors by the encoding scheme below: a real color
// always has a "kind" tag in bits 24-31 of 0x01, 0x02, or 0x03, which never
// equals the all-ones sentinel.
inline constexpr uint32_t default_fg = 0xFFFFFFFFu;
inline constexpr uint32_t default_bg = 0xFFFFFFFFu;

enum TextStyleFlags : uint8_t {
    style_bold = 1u << 0,
    style_underline = 1u << 1,
    style_italic = 1u << 2,
    style_inverse = 1u << 3,
    style_blink = 1u << 4,
};

// fg/bg color encoding: bits 24-31 select the kind, bits 0-23 hold the value.
//   kind 1 (standard): value 0-15 is one of the 16 ANSI colors.
//   kind 2 (256-color): value 0-255 is the palette index.
//   kind 3 (truecolor): value is 0xRRGGBB.
// default_fg/default_bg (0xFFFFFFFF) mean "terminal default", not a kind.
inline constexpr uint32_t color_kind_standard = 1u;
inline constexpr uint32_t color_kind_256 = 2u;
inline constexpr uint32_t color_kind_truecolor = 3u;

inline constexpr uint32_t encode_standard_color(uint8_t index) {
    return (color_kind_standard << 24) | index;
}
inline constexpr uint32_t encode_256_color(uint8_t index) {
    return (color_kind_256 << 24) | index;
}
inline constexpr uint32_t encode_truecolor(uint8_t r, uint8_t g, uint8_t b) {
    return (color_kind_truecolor << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}

struct TextStyle {
    uint32_t fg = default_fg;
    uint32_t bg = default_bg;
    uint8_t flags = 0;

    friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

struct StyledSpan {
    TextStyle style;
    uint32_t start = 0;
    uint32_t length = 0;

    friend bool operator==(const StyledSpan&, const StyledSpan&) = default;
};

struct StyledLine {
    std::string plain; // ANSI-stripped; used for trigger matching
    std::vector<StyledSpan> spans; // styling over `plain`; used for display
    bool is_prompt = false;

    friend bool operator==(const StyledLine&, const StyledLine&) = default;
};

// ---- Engine-level events ----
// Produced on the network thread (or the stdin thread for UserInput),
// consumed only on the engine thread via EventQueue::pop_wait.

struct Connected {};

struct Disconnected {
    std::string reason;
};

struct LineReceived {
    StyledLine line;
};

struct GmcpReceived {
    std::string package;
    nlohmann::json json;
};

struct UserInput {
    std::string text;
};

struct TimerFired {
    uint64_t id = 0;
};

using Event = std::variant<Connected, Disconnected, LineReceived, GmcpReceived, UserInput, TimerFired>;

} // namespace mudclient
