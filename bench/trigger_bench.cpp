// Measures TriggerManager throughput with 200 synthetic triggers against a
// mixed corpus of matching/non-matching lines, per docs/SPEC.md section 3
// ("Target: 200 active triggers at 2,000 lines/second on one core").
//
// Usage: trigger_bench [--min-lines-per-sec N] [--lines N]
// Exits nonzero if measured throughput is below the threshold.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "mudclient/events.hpp"
#include "mudclient/trigger_manager.hpp"

using namespace mudclient;

namespace {

// Builds 200 triggers: most have a realistic, extractable required literal
// (a unique tag, mimicking distinct combat/room/status message shapes so
// the prefilter can do useful work), a minority intentionally have no
// extractable literal (e.g. bare capture groups) to also exercise the
// always-scanned bucket honestly rather than benchmarking a best case.
void build_synthetic_triggers(TriggerManager& triggers, int count) {
    for (int i = 0; i < count; ++i) {
        TriggerManager::Options opts;
        opts.priority = i % 5;
        std::string pattern;
        if (i % 10 == 0) {
            // No extractable literal: pure capture, always scanned.
            pattern = "^(\\w+)$";
        } else {
            pattern = "^Event" + std::to_string(i) + ": (\\w+) triggered condition " + std::to_string(i) + "$";
        }
        triggers.add_trigger(pattern, [](const std::vector<std::string>&) {}, opts);
    }
}

// A corpus dominated by lines that resemble ordinary MUD output (so most
// prefilters correctly reject them without a regex call), with roughly 5%
// of lines constructed to actually match one of the synthetic triggers, so
// the benchmark isn't measuring a degenerate all-miss case either.
std::vector<std::string> build_corpus(size_t count, int trigger_count) {
    std::vector<std::string> lines;
    lines.reserve(count);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> word_len(3, 10);
    std::uniform_int_distribution<int> word_count(4, 12);
    std::uniform_int_distribution<int> trigger_pick(0, trigger_count - 1);
    std::uniform_int_distribution<int> hit_roll(0, 99);
    const char* alphabet = "abcdefghijklmnopqrstuvwxyz";

    for (size_t i = 0; i < count; ++i) {
        if (hit_roll(rng) < 5) {
            int idx = trigger_pick(rng);
            lines.push_back("Event" + std::to_string(idx) + ": someone triggered condition " + std::to_string(idx));
            continue;
        }
        std::string line;
        int words = word_count(rng);
        for (int w = 0; w < words; ++w) {
            if (w > 0) line += ' ';
            int len = word_len(rng);
            for (int c = 0; c < len; ++c) {
                line += alphabet[rng() % 26];
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

int main(int argc, char** argv) {
    double min_lines_per_sec = 2000.0;
    size_t total_lines = 20000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--min-lines-per-sec") == 0 && i + 1 < argc) {
            min_lines_per_sec = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--lines") == 0 && i + 1 < argc) {
            total_lines = static_cast<size_t>(std::atoll(argv[++i]));
        }
    }

    constexpr int kTriggerCount = 200;
    TriggerManager triggers;
    build_synthetic_triggers(triggers, kTriggerCount);
    std::vector<std::string> corpus = build_corpus(total_lines, kTriggerCount);

    auto start = std::chrono::steady_clock::now();
    size_t total_fired = 0;
    for (auto& text : corpus) {
        StyledLine line;
        line.plain = text;
        auto outcome = triggers.process_line(line);
        total_fired += static_cast<size_t>(outcome.fired_count);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    double seconds = std::chrono::duration<double>(elapsed).count();
    double lines_per_sec = seconds > 0 ? static_cast<double>(total_lines) / seconds : 0.0;

    std::printf("trigger_bench: %zu triggers=%d lines=%zu elapsed=%.3fs throughput=%.1f lines/sec fired=%zu\n",
                triggers.count(), kTriggerCount, total_lines, seconds, lines_per_sec, total_fired);

    if (lines_per_sec < min_lines_per_sec) {
        std::fprintf(stderr, "trigger_bench: FAIL: %.1f lines/sec < required %.1f lines/sec\n", lines_per_sec,
                     min_lines_per_sec);
        return 1;
    }
    std::printf("trigger_bench: PASS (>= %.1f lines/sec)\n", min_lines_per_sec);
    return 0;
}
