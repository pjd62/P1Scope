#pragma once

#include <ctime>
#include <iomanip>
#include <iostream>
#include <syncstream>

// Emit each complete message atomically so messages from different threads do not interleave.
inline std::osyncstream logStderr() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::osyncstream message(std::cerr);
    message << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << "] ";
    return message;
}
