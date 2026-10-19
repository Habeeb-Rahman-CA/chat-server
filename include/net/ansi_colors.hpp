#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace net::ui {

// ANSI escape sequences
inline constexpr std::string_view RESET       = "\033[0m";
inline constexpr std::string_view BOLD        = "\033[1m";
inline constexpr std::string_view DIM         = "\033[2m";
inline constexpr std::string_view RED         = "\033[31m";
inline constexpr std::string_view GREEN       = "\033[32m";
inline constexpr std::string_view YELLOW      = "\033[33m";
inline constexpr std::string_view BLUE        = "\033[34m";
inline constexpr std::string_view MAGENTA     = "\033[35m";
inline constexpr std::string_view CYAN        = "\033[36m";
inline constexpr std::string_view WHITE       = "\033[37m";
inline constexpr std::string_view BRIGHT_RED     = "\033[91m";
inline constexpr std::string_view BRIGHT_GREEN   = "\033[92m";
inline constexpr std::string_view BRIGHT_YELLOW  = "\033[93m";
inline constexpr std::string_view BRIGHT_BLUE    = "\033[94m";
inline constexpr std::string_view BRIGHT_MAGENTA = "\033[95m";
inline constexpr std::string_view BRIGHT_CYAN    = "\033[96m";
inline constexpr std::string_view CLEAR_LINE     = "\r\033[K";

// Deterministically picks an ANSI color for a username so each friend has a recognizable color
inline std::string_view color_for_name(std::string_view name) {
    constexpr std::string_view PALETTE[] = {
        BRIGHT_GREEN,
        BRIGHT_YELLOW,
        BRIGHT_BLUE,
        BRIGHT_MAGENTA,
        BRIGHT_CYAN,
        GREEN,
        YELLOW,
        BLUE,
        MAGENTA,
        CYAN
    };
    uint32_t hash = 5381;
    for (char c : name) {
        hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
    }
    return PALETTE[hash % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
}

// Current local time formatted as [HH:MM:SS]
inline std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    ::localtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "]";
    return oss.str();
}

// Strips dangerous ANSI escape sequences from user messages to prevent terminal injection
inline std::string sanitize_message(std::string_view input) {
    std::string clean;
    clean.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '\033') {
            // Drop ESC character and skip subsequent sequence
            continue;
        }
        // Disallow other non-printable ASCII control characters except space and tab
        if (static_cast<unsigned char>(c) < 32 && c != '\t') {
            continue;
        }
        clean.push_back(c);
    }
    return clean;
}

} // namespace net::ui

