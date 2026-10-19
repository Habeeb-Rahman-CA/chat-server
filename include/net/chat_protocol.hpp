#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace net {

// Wire Framing Specification:
// Every message is a single line terminated by '\n' (with optional '\r').
// Maximum allowed frame size is 4096 bytes to prevent memory exhaustion attacks.
constexpr size_t MAX_FRAME_SIZE = 4096;

// Client Commands parsed from the wire
struct NickCmd {
    std::string username;
};

struct MsgCmd {
    std::string text;
};

struct WhisperCmd {
    std::string target;
    std::string text;
};

struct UsersCmd {};
struct PingCmd {
    std::string payload;
};
struct QuitCmd {};

struct MalformedCmd {
    std::string reason;
};

using ClientCommand = std::variant<
    NickCmd,
    MsgCmd,
    WhisperCmd,
    UsersCmd,
    PingCmd,
    QuitCmd,
    MalformedCmd
>;

class ChatProtocol {
public:
    // Attempts to extract the next complete frame from an accumulator buffer.
    // Returns std::nullopt if no complete newline-delimited frame is present yet.
    // If a frame exceeds MAX_FRAME_SIZE without a newline, it truncates/evicts to protect memory.
    static std::optional<std::string> extract_next_frame(std::string& in_buffer, bool& oversized_error) {
        oversized_error = false;
        size_t newline_pos = in_buffer.find('\n');

        if (newline_pos == std::string::npos) {
            if (in_buffer.size() > MAX_FRAME_SIZE) {
                // Buffer exceeded maximum allowable frame without a delimiter
                oversized_error = true;
                in_buffer.clear();
            }
            return std::nullopt;
        }

        if (newline_pos > MAX_FRAME_SIZE) {
            oversized_error = true;
            in_buffer.erase(0, newline_pos + 1);
            return std::nullopt;
        }

        std::string raw_frame = in_buffer.substr(0, newline_pos);
        in_buffer.erase(0, newline_pos + 1);

        // Strip trailing carriage return if present (\r\n)
        if (!raw_frame.empty() && raw_frame.back() == '\r') {
            raw_frame.pop_back();
        }

        return raw_frame;
    }

    // Parses a single raw frame into a structured ClientCommand
    static ClientCommand parse_command(std::string_view frame) {
        // Strip leading whitespace
        while (!frame.empty() && (frame.front() == ' ' || frame.front() == '\t')) {
            frame.remove_prefix(1);
        }
        if (frame.empty()) {
            return MalformedCmd{"Empty command line"};
        }

        size_t space_idx = frame.find(' ');
        std::string_view verb = frame.substr(0, space_idx);
        std::string_view args = (space_idx == std::string_view::npos) ? "" : frame.substr(space_idx + 1);

        // Strip leading whitespace from arguments
        while (!args.empty() && args.front() == ' ') {
            args.remove_prefix(1);
        }

        if (verb == "NICK") {
            if (args.empty() || args.find(' ') != std::string_view::npos) {
                return MalformedCmd{"NICK command requires a single username argument without spaces"};
            }
            return NickCmd{std::string(args)};
        } else if (verb == "MSG") {
            if (args.empty()) {
                return MalformedCmd{"MSG command requires non-empty message text"};
            }
            return MsgCmd{std::string(args)};
        } else if (verb == "WHISPER") {
            size_t target_sep = args.find(' ');
            if (target_sep == std::string_view::npos || target_sep == 0) {
                return MalformedCmd{"WHISPER requires: WHISPER <target_user> <message>"};
            }
            std::string target(args.substr(0, target_sep));
            std::string_view msg_part = args.substr(target_sep + 1);
            while (!msg_part.empty() && msg_part.front() == ' ') msg_part.remove_prefix(1);
            if (msg_part.empty()) {
                return MalformedCmd{"WHISPER message body cannot be empty"};
            }
            return WhisperCmd{std::move(target), std::string(msg_part)};
        } else if (verb == "USERS") {
            return UsersCmd{};
        } else if (verb == "PING") {
            return PingCmd{std::string(args)};
        } else if (verb == "QUIT") {
            return QuitCmd{};
        }

        return MalformedCmd{"Unknown command verb: " + std::string(verb)};
    }

    // Server-to-Client Serializers
    static std::string serialize_ok(std::string_view msg = "") {
        if (msg.empty()) return "OK\n";
        return "OK " + std::string(msg) + "\n";
    }

    static std::string serialize_error(int code, std::string_view desc) {
        return "ERR " + std::to_string(code) + " " + std::string(desc) + "\n";
    }

    static std::string serialize_chat(std::string_view sender, std::string_view text) {
        return "CHAT " + std::string(sender) + " " + std::string(text) + "\n";
    }

    static std::string serialize_whisper(std::string_view from_user, std::string_view text) {
        return "WHISPER_FROM " + std::string(from_user) + " " + std::string(text) + "\n";
    }

    static std::string serialize_join(std::string_view username) {
        return "JOIN " + std::string(username) + "\n";
    }

    static std::string serialize_leave(std::string_view username) {
        return "LEAVE " + std::string(username) + "\n";
    }

    static std::string serialize_pong(std::string_view payload) {
        if (payload.empty()) return "PONG\n";
        return "PONG " + std::string(payload) + "\n";
    }

    static std::string serialize_users_list(const std::vector<std::string>& users) {
        std::string res = "USERS " + std::to_string(users.size());
        for (const auto& u : users) {
            res += " " + u;
        }
        res += "\n";
        return res;
    }
};

} // namespace net

