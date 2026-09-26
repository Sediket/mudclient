#pragma once

#include <chrono>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

// Writes the --record file: one JSON object per line (JSON Lines), one
// per raw inbound chunk or outbound line, in chronological order.
// Schema (deliberately simple; read back by tests/fake_mud):
//   {"dir":"in",  "t_ms":<int>, "data_b64":"<base64 of the raw chunk>"}
//   {"dir":"out", "t_ms":<int>, "line":"<the outbound line, as text>"}
// t_ms is milliseconds elapsed since the recorder was constructed
// (i.e. since Connected), preserving relative timing; data_b64 preserves
// the exact chunk boundaries the socket delivered.
namespace mudclient {

class SessionRecorder {
public:
    explicit SessionRecorder(const std::string& path);

    void record_inbound(std::span<const uint8_t> data);
    void record_outbound_line(std::string_view line);

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::ofstream out_;
};

std::string base64_encode(std::span<const uint8_t> data);
std::string base64_decode(std::string_view text);

} // namespace mudclient
