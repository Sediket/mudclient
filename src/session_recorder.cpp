#include "mudclient/session_recorder.hpp"

#include <array>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace mudclient {

namespace {
constexpr std::string_view kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64_encode(std::span<const uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
        out.push_back(kBase64Chars[n & 0x3F]);
    }
    size_t remaining = data.size() - i;
    if (remaining == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out += "==";
    } else if (remaining == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(std::string_view text) {
    std::array<int8_t, 256> table;
    table.fill(-1);
    for (size_t i = 0; i < kBase64Chars.size(); ++i) {
        table[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
    }
    std::string out;
    out.reserve(text.size() / 4 * 3);
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        int8_t value = table[static_cast<uint8_t>(c)];
        if (value < 0) {
            continue; // skip whitespace/unknown characters defensively
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

SessionRecorder::SessionRecorder(const std::string& path) : out_(path, std::ios::trunc) {
    if (!out_) {
        throw std::runtime_error("cannot open record file: " + path);
    }
}

void SessionRecorder::record_inbound(std::span<const uint8_t> data) {
    auto t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
    nlohmann::json entry = {{"dir", "in"}, {"t_ms", t_ms.count()}, {"data_b64", base64_encode(data)}};
    out_ << entry.dump() << '\n';
    out_.flush();
}

void SessionRecorder::record_outbound_line(std::string_view line) {
    auto t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_);
    nlohmann::json entry = {{"dir", "out"}, {"t_ms", t_ms.count()}, {"line", std::string(line)}};
    out_ << entry.dump() << '\n';
    out_.flush();
}

} // namespace mudclient
