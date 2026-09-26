#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>
#include <vector>

#include "mudclient/telnet_parser.hpp"

using mudclient::encode_256_color;
using mudclient::encode_standard_color;
using mudclient::encode_truecolor;
using mudclient::GmcpMessage;
using mudclient::StyledLine;
using mudclient::StyledSpan;
using mudclient::TelnetParser;
using mudclient::TextStyle;

namespace {

constexpr char IAC = '\xFF';
constexpr char DONT = '\xFE';
constexpr char DO = '\xFD';
constexpr char WONT = '\xFC';
constexpr char WILL = '\xFB';
constexpr char SB = '\xFA';
constexpr char GA = '\xF9';
constexpr char SE = '\xF0';
constexpr char EOR = '\xEF';
constexpr char GMCP = '\xC9';
constexpr char NAWS = '\x1F';
constexpr char TTYPE = '\x18';
constexpr char OPT_EOR = '\x19';

struct Result {
    std::vector<TelnetParser::OutputEvent> events;
    std::string sent;
    bool operator==(const Result&) const = default;
};

std::span<const uint8_t> as_bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Feeds `input` split at the given chunk boundaries (sorted offsets).
Result feed_chunks(std::string_view input, const std::vector<size_t>& cuts) {
    TelnetParser parser;
    Result r;
    size_t prev = 0;
    for (size_t cut : cuts) {
        parser.feed(as_bytes(input.substr(prev, cut - prev)), r.events, r.sent);
        prev = cut;
    }
    parser.feed(as_bytes(input.substr(prev)), r.events, r.sent);
    return r;
}

Result feed_whole(std::string_view input) { return feed_chunks(input, {}); }

// Core resumability check: the result of feeding `input` in one call must
// equal the result of splitting it at every single byte offset, of feeding
// it one byte at a time, and of a set of seeded random multi-way splits.
Result check_every_split(std::string_view input) {
    const Result whole = feed_whole(input);
    for (size_t cut = 1; cut < input.size(); ++cut) {
        INFO("two-way split at offset " << cut);
        REQUIRE(feed_chunks(input, {cut}) == whole);
    }
    std::vector<size_t> every;
    for (size_t i = 1; i < input.size(); ++i) {
        every.push_back(i);
    }
    REQUIRE(feed_chunks(input, every) == whole);

    std::mt19937 rng(12345);
    for (int trial = 0; trial < 50 && input.size() > 2; ++trial) {
        std::vector<size_t> cuts;
        std::uniform_int_distribution<size_t> pick(1, input.size() - 1);
        size_t n = 1 + static_cast<size_t>(trial % 7);
        for (size_t k = 0; k < n; ++k) {
            cuts.push_back(pick(rng));
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        INFO("random split trial " << trial);
        REQUIRE(feed_chunks(input, cuts) == whole);
    }
    return whole;
}

const StyledLine& line_at(const Result& r, size_t i) {
    REQUIRE(i < r.events.size());
    REQUIRE(std::holds_alternative<StyledLine>(r.events[i]));
    return std::get<StyledLine>(r.events[i]);
}

std::string gmcp_hello_bytes() {
    std::string s;
    s += std::string{IAC, SB, GMCP} + "Core.Hello {\"client\":\"mudclient\",\"version\":\"0.1.0\"}" + std::string{IAC, SE};
    s += std::string{IAC, SB, GMCP} + "Core.Supports.Set [\"Core 1\"]" + std::string{IAC, SE};
    return s;
}

} // namespace

TEST_CASE("plain lines split on LF with trailing CR stripped", "[parser][split]") {
    auto r = check_every_split("Hello\r\nWorld\nthird\r\n");
    REQUIRE(r.events.size() == 3);
    CHECK(line_at(r, 0).plain == "Hello");
    CHECK(line_at(r, 1).plain == "World");
    CHECK(line_at(r, 2).plain == "third");
    CHECK_FALSE(line_at(r, 0).is_prompt);
    CHECK(line_at(r, 0).spans == std::vector<StyledSpan>{StyledSpan{TextStyle{}, 0, 5}});
    CHECK(r.sent.empty());
}

TEST_CASE("IAC IAC in data yields a literal 0xFF", "[parser][split]") {
    std::string in = std::string("a") + IAC + IAC + "b\n";
    auto r = check_every_split(in);
    REQUIRE(r.events.size() == 1);
    CHECK(line_at(r, 0).plain == std::string("a\xFF" "b"));
}

TEST_CASE("IAC GA and IAC EOR complete a prompt line", "[parser][split]") {
    std::string in = std::string("HP:10> ") + IAC + GA + "Name: " + IAC + EOR + "next\n";
    auto r = check_every_split(in);
    REQUIRE(r.events.size() == 3);
    CHECK(line_at(r, 0).plain == "HP:10> ");
    CHECK(line_at(r, 0).is_prompt);
    CHECK(line_at(r, 1).plain == "Name: ");
    CHECK(line_at(r, 1).is_prompt);
    CHECK(line_at(r, 2).plain == "next");
    CHECK_FALSE(line_at(r, 2).is_prompt);
}

TEST_CASE("GMCP: WILL accepted with DO, hello sent, SB parsed", "[parser][split][gmcp]") {
    std::string in = std::string{IAC, WILL, GMCP} + "text\n" + std::string{IAC, SB, GMCP} +
                     "Char.Vitals {\"hp\": 10, \"maxhp\": 20}" + std::string{IAC, SE} + "after\n";
    auto r = check_every_split(in);
    CHECK(r.sent == std::string{IAC, DO, GMCP} + gmcp_hello_bytes());
    REQUIRE(r.events.size() == 3);
    CHECK(line_at(r, 0).plain == "text");
    REQUIRE(std::holds_alternative<GmcpMessage>(r.events[1]));
    const auto& g = std::get<GmcpMessage>(r.events[1]);
    CHECK(g.package == "Char.Vitals");
    CHECK(g.json["hp"] == 10);
    CHECK(g.json["maxhp"] == 20);
    CHECK(line_at(r, 2).plain == "after");
}

TEST_CASE("GMCP: package without payload and invalid JSON", "[parser][gmcp]") {
    std::string in = std::string{IAC, SB, GMCP} + "Core.Ping" + std::string{IAC, SE} + std::string{IAC, SB, GMCP} +
                     "Bad.Json {not json" + std::string{IAC, SE};
    auto r = check_every_split(in);
    REQUIRE(r.events.size() == 2);
    const auto& ping = std::get<GmcpMessage>(r.events[0]);
    CHECK(ping.package == "Core.Ping");
    CHECK(ping.json.is_object());
    const auto& bad = std::get<GmcpMessage>(r.events[1]);
    CHECK(bad.package == "Bad.Json");
    CHECK(bad.json.is_discarded());
}

TEST_CASE("IAC IAC inside a subnegotiation is a literal 0xFF, not SB end", "[parser][split][gmcp]") {
    // A literal 0xFF in the package name: if IAC IAC were mishandled the SB
    // would terminate early and the tail would leak into text.
    std::string in = std::string{IAC, SB, GMCP} + "Pkg" + IAC + IAC + "X {\"a\":[1,2]}" + std::string{IAC, SE} + "t\n";
    auto r = check_every_split(in);
    REQUIRE(r.events.size() == 2);
    const auto& g = std::get<GmcpMessage>(r.events[0]);
    CHECK(g.package == std::string("Pkg\xFFX"));
    CHECK(g.json["a"] == nlohmann::json::array({1, 2}));
    CHECK(line_at(r, 1).plain == "t");
}

TEST_CASE("NAWS: DO answered with WILL and window size", "[parser][split][negotiation]") {
    std::string in = std::string{IAC, DO, NAWS};
    auto r = check_every_split(in);
    CHECK(r.sent == std::string{IAC, WILL, NAWS, IAC, SB, NAWS, 0, 80, 0, 24, IAC, SE});
}

TEST_CASE("NAWS: size byte equal to 255 is escaped", "[parser][negotiation]") {
    TelnetParser parser(255, 24);
    Result r;
    std::string in = std::string{IAC, DO, NAWS};
    parser.feed(as_bytes(in), r.events, r.sent);
    CHECK(r.sent == std::string{IAC, WILL, NAWS, IAC, SB, NAWS, 0, IAC, IAC, 0, 24, IAC, SE});
}

TEST_CASE("TTYPE: DO answered with WILL, SEND answered with IS", "[parser][split][negotiation]") {
    std::string in = std::string{IAC, DO, TTYPE, IAC, SB, TTYPE, 1, IAC, SE};
    auto r = check_every_split(in);
    CHECK(r.sent == std::string{IAC, WILL, TTYPE, IAC, SB, TTYPE, 0} + "MUDCLIENT" + std::string{IAC, SE});
}

TEST_CASE("EOR is accepted, unknown options and MCCP are refused", "[parser][split][negotiation]") {
    std::string in = std::string{IAC, WILL, OPT_EOR, IAC, WILL, '\x56', IAC, WILL, '\x55', IAC, DO, '\x01', IAC,
                                 DO, '\x56'};
    auto r = check_every_split(in);
    CHECK(r.sent == std::string{IAC, DO, OPT_EOR, IAC, DONT, '\x56', IAC, DONT, '\x55', IAC, WONT, '\x01', IAC, WONT,
                                '\x56'});
}

TEST_CASE("Q-method: repeated WILL/DO does not loop; WONT after enable is acknowledged", "[parser][negotiation]") {
    std::string in = std::string{IAC, WILL, GMCP, IAC, WILL, GMCP, IAC, DO, NAWS, IAC, DO, NAWS, IAC, WONT, GMCP,
                                 IAC, WONT, GMCP, IAC, DONT, NAWS, IAC, DONT, NAWS};
    auto r = check_every_split(in);
    std::string expected = std::string{IAC, DO, GMCP} + gmcp_hello_bytes() +
                           std::string{IAC, WILL, NAWS, IAC, SB, NAWS, 0, 80, 0, 24, IAC, SE} +
                           std::string{IAC, DONT, GMCP} + std::string{IAC, WONT, NAWS};
    CHECK(r.sent == expected);
}

TEST_CASE("ANSI SGR produces styled spans over the plain text", "[parser][split][ansi]") {
    auto r = check_every_split("\x1b[1;32mGreen\x1b[0m plain\n");
    REQUIRE(r.events.size() == 1);
    const auto& line = line_at(r, 0);
    CHECK(line.plain == "Green plain");
    TextStyle green{.fg = encode_standard_color(2), .bg = mudclient::default_bg, .flags = mudclient::style_bold};
    REQUIRE(line.spans.size() == 2);
    CHECK(line.spans[0] == StyledSpan{green, 0, 5});
    CHECK(line.spans[1] == StyledSpan{TextStyle{}, 5, 6});
}

TEST_CASE("ANSI: bright, 256-color and truecolor, fg and bg", "[parser][split][ansi]") {
    auto r = check_every_split("\x1b[91;104ma\x1b[38;5;208;48;5;17mb\x1b[38;2;10;20;30;48;2;1;2;3mc\x1b[39;49md\n");
    const auto& line = line_at(r, 0);
    CHECK(line.plain == "abcd");
    REQUIRE(line.spans.size() == 4);
    CHECK(line.spans[0].style.fg == encode_standard_color(9));
    CHECK(line.spans[0].style.bg == encode_standard_color(12));
    CHECK(line.spans[1].style.fg == encode_256_color(208));
    CHECK(line.spans[1].style.bg == encode_256_color(17));
    CHECK(line.spans[2].style.fg == encode_truecolor(10, 20, 30));
    CHECK(line.spans[2].style.bg == encode_truecolor(1, 2, 3));
    CHECK(line.spans[3].style == TextStyle{});
}

TEST_CASE("ANSI: attribute set/reset codes", "[parser][ansi]") {
    auto r = check_every_split("\x1b[1;3;4;5;7mA\x1b[22;23;24;25;27mB\x1b[mC\n");
    const auto& line = line_at(r, 0);
    REQUIRE(line.spans.size() == 2); // B and C share the default style
    CHECK(line.spans[0].style.flags == (mudclient::style_bold | mudclient::style_italic | mudclient::style_underline |
                                        mudclient::style_blink | mudclient::style_inverse));
    CHECK(line.spans[1] == StyledSpan{TextStyle{}, 1, 2});
}

TEST_CASE("ANSI: truncated extended-color sequence abandons the rest of the SGR, not reinterpreted", "[parser][ansi]") {
    // "38;5" with no index must not fall through and reinterpret the "5" as
    // a bare SGR code (blink). The whole sequence is simply dropped.
    auto r = check_every_split("\x1b[38;5mA\x1b[38;2;9;9mB\n");
    const auto& line = line_at(r, 0);
    CHECK(line.plain == "AB");
    REQUIRE(line.spans.size() == 1);
    CHECK(line.spans[0].style.flags == 0);
    CHECK(line.spans[0].style.fg == mudclient::default_fg);
}

TEST_CASE("ANSI: non-SGR CSI and bare ESC are consumed silently", "[parser][split][ansi]") {
    auto r = check_every_split("\x1b[2J\x1b[10;5Habc\x1b[?25l\x1b" "Xd\n");
    CHECK(line_at(r, 0).plain == "abcXd");
}

TEST_CASE("ANSI: style persists across lines", "[parser][split][ansi]") {
    auto r = check_every_split("\x1b[31mred\nstill red\n\x1b[0mplain\n");
    REQUIRE(r.events.size() == 3);
    TextStyle red{.fg = encode_standard_color(1)};
    CHECK(line_at(r, 0).spans == std::vector<StyledSpan>{StyledSpan{red, 0, 3}});
    CHECK(line_at(r, 1).spans == std::vector<StyledSpan>{StyledSpan{red, 0, 9}});
    CHECK(line_at(r, 2).spans == std::vector<StyledSpan>{StyledSpan{TextStyle{}, 0, 5}});
}

TEST_CASE("UTF-8: multibyte codepoints survive every split and span edges align", "[parser][split][utf8]") {
    // é (2 bytes), € (3 bytes), 😀 (4 bytes), styled differently.
    std::string in = "\x1b[31m\xC3\xA9\xE2\x82\xAC\x1b[32m\xF0\x9F\x98\x80\x1b[0mx\n";
    auto r = check_every_split(in);
    const auto& line = line_at(r, 0);
    CHECK(line.plain == "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80x");
    REQUIRE(line.spans.size() == 3);
    CHECK(line.spans[0] == StyledSpan{TextStyle{.fg = encode_standard_color(1)}, 0, 5});
    CHECK(line.spans[1] == StyledSpan{TextStyle{.fg = encode_standard_color(2)}, 5, 4});
    CHECK(line.spans[2] == StyledSpan{TextStyle{}, 9, 1});
    for (const auto& span : line.spans) {
        // A span must never begin on a UTF-8 continuation byte.
        CHECK((static_cast<uint8_t>(line.plain[span.start]) & 0xC0) != 0x80);
    }
}

TEST_CASE("Subnegotiation overflow over 64KB is discarded and counted", "[parser][overflow]") {
    TelnetParser parser;
    Result r;
    std::string big(70000, 'x');
    std::string in = std::string{IAC, SB, GMCP} + "Big.Pkg " + big + std::string{IAC, SE} + "ok\n";
    // Feed in uneven chunks to cross the cap mid-chunk.
    for (size_t pos = 0; pos < in.size(); pos += 4093) {
        parser.feed(as_bytes(std::string_view(in).substr(pos, 4093)), r.events, r.sent);
    }
    CHECK(parser.overflow_count() == 1);
    REQUIRE(r.events.size() == 1);
    CHECK(line_at(r, 0).plain == "ok");

    // Parser remains usable for a normal SB afterwards.
    std::string next = std::string{IAC, SB, GMCP} + "Small {}" + std::string{IAC, SE};
    parser.feed(as_bytes(next), r.events, r.sent);
    REQUIRE(r.events.size() == 2);
    CHECK(std::get<GmcpMessage>(r.events[1]).package == "Small");
}

TEST_CASE("Subnegotiation exactly at the 64KB cap is delivered", "[parser][overflow]") {
    TelnetParser parser;
    Result r;
    std::string prefix = "P ";
    std::string body = "\"" + std::string(64 * 1024 - prefix.size() - 2, 'y') + "\"";
    std::string in = std::string{IAC, SB, GMCP} + prefix + body + std::string{IAC, SE};
    parser.feed(as_bytes(in), r.events, r.sent);
    CHECK(parser.overflow_count() == 0);
    REQUIRE(r.events.size() == 1);
    CHECK(std::get<GmcpMessage>(r.events[0]).json.is_string());
}

TEST_CASE("Malformed SB interrupted by IAC command resynchronizes", "[parser][split]") {
    std::string in = std::string{IAC, SB, GMCP} + "Partial" + std::string{IAC, WILL, OPT_EOR} + "line\n";
    auto r = check_every_split(in);
    REQUIRE(r.events.size() == 1);
    CHECK(line_at(r, 0).plain == "line");
    CHECK(r.sent == std::string{IAC, DO, OPT_EOR});
}

TEST_CASE("Idle flush turns a partial line into a prompt", "[parser][prompt]") {
    TelnetParser parser;
    Result r;
    std::vector<TelnetParser::OutputEvent> flushed;
    CHECK_FALSE(parser.flush_idle(flushed));
    parser.feed(as_bytes("\x1b[33mEnter name: "), r.events, r.sent);
    CHECK(r.events.empty());
    CHECK(parser.has_pending_partial_line());
    REQUIRE(parser.flush_idle(flushed));
    REQUIRE(flushed.size() == 1);
    const auto& line = std::get<StyledLine>(flushed[0]);
    CHECK(line.plain == "Enter name: ");
    CHECK(line.is_prompt);
    CHECK(line.spans == std::vector<StyledSpan>{StyledSpan{TextStyle{.fg = encode_standard_color(3)}, 0, 12}});
    CHECK_FALSE(parser.has_pending_partial_line());
    CHECK_FALSE(parser.flush_idle(flushed));
}

TEST_CASE("Combined corpus survives every split", "[parser][split]") {
    std::string in = std::string{IAC, WILL, GMCP, IAC, DO, NAWS, IAC, DO, TTYPE} + "\x1b[1;34mWelcome\x1b[0m\r\n" +
                     std::string{IAC, SB, TTYPE, 1, IAC, SE} + "caf\xC3\xA9 " + IAC + IAC + "\r\n" +
                     std::string{IAC, SB, GMCP} + "Room.Info {\"exits\":{\"n\":1}}" + std::string{IAC, SE} +
                     "\x1b[38;5;100mprompt>\x1b[0m " + std::string{IAC, GA};
    auto r = check_every_split(in);
    REQUIRE(r.events.size() == 4);
    CHECK(line_at(r, 0).plain == "Welcome");
    CHECK(line_at(r, 1).plain == "caf\xC3\xA9 \xFF");
    CHECK(std::get<GmcpMessage>(r.events[2]).json["exits"]["n"] == 1);
    CHECK(line_at(r, 3).plain == "prompt> ");
    CHECK(line_at(r, 3).is_prompt);
}
