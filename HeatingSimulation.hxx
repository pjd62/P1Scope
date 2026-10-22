#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// A shared deterministic model keeps simulated gas and Hue temperatures in sync.
namespace HeatingSimulation {
inline const auto started = std::chrono::system_clock::now();
inline double elapsed() {
    return std::max(0.0, std::chrono::duration<double>(
        std::chrono::system_clock::now() - started).count());
}
inline double livingRoom(double seconds) {
    seconds = std::max(0.0, seconds);
    // Initial warm-up: 18 to 21 C over one hour.
    if (seconds < 3600.0) return 18.0 + seconds / 1200.0;
    // Start the thermostat cycle with cooling after reaching 21 C.
    const double phase = std::fmod(seconds - 3600.0 + 600.0, 1200.0);
    // Heat from 20.5 to 21 C, then cool to 20.5 C with the burner off.
    return phase < 600.0 ? 20.5 + phase / 1200.0 : 21.0 - (phase - 600.0) / 1200.0;
}
inline double gasTotal(double seconds) {
    seconds = std::max(0.0, seconds);
    const double afterWarmup = std::max(0.0, seconds - 3600.0);
    const double heatingSeconds = std::min(seconds, 3600.0) +
        std::floor(afterWarmup / 1200.0) * 600.0 +
        std::max(0.0, std::fmod(afterWarmup, 1200.0) - 600.0);
    return 5846.926 + heatingSeconds * (0.9 / 3600.0);
}
inline double roomTemperature(int room, double seconds) {
    const double living = livingRoom(seconds);
    if (room == 1) return living;
    return living - (room == 0 ? 2.0 : 1.0);
}
}
