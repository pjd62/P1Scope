#pragma once

#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <syncstream>

namespace logging {
inline std::mutex mutex;
inline std::deque<std::string> messages;
inline size_t bytes = 0;
inline constexpr size_t maxBytes = 256 * 1024;
inline constexpr size_t maxMessages = 1000;

// Keep bounded diagnostic history while preserving the normal stderr output.
class Message : public std::ostringstream {
public:
    Message() {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        *this << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << "] ";
    }
    ~Message() {
        try {
            auto text = str();
            std::lock_guard lock(mutex);
            std::osyncstream(std::cerr) << text;
            if (text.size() > maxBytes) text = text.substr(0, maxBytes - 16) + " [truncated]\n";
            messages.push_back(std::move(text));
            bytes += messages.back().size();
            while (messages.size() > maxMessages || bytes > maxBytes) {
                bytes -= messages.front().size();
                messages.pop_front();
            }
        } catch (...) { /* Logging must not throw during stack unwinding. */ }
    }
};
}

inline logging::Message logStderr() { return logging::Message{}; }

inline std::string recentStderr() {
    std::lock_guard lock(logging::mutex);
    std::string text;
    text.reserve(logging::bytes);
    for (const auto &message : logging::messages) text += message;
    return text;
}
