// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <istream>
#include <string>

namespace modern {
// Circular deadzone preserves direction, with continuous travel outside it.
struct Axis {
    float x, y;
};

inline Axis stick(int x, int y, float deadzone) {
    float a = std::clamp((x - 128) / 127.f, -1.f, 1.f);
    float b = std::clamp((y - 128) / 127.f, -1.f, 1.f);
    float length = std::hypot(a, b);
    if (length <= deadzone) {
        return {};
    }
    float magnitude = (std::min(length, 1.f) - deadzone) / (1.f - deadzone);
    return {a / length * magnitude, b / length * magnitude};
}

inline float mouse(float counts, int percent) {
    return std::isfinite(counts) ? counts * .0015f * percent / 100.f : 0.f;
}

inline float rate(float axis, int degrees) {
    // OpenRAC runs both RAC1 builds at 60 ticks/s (docs/port/speedrun/RULES.md).
    return axis * degrees * .01745329252f / 60.f;
}

// 50 retains the base speed; each of the 100 positions is distinct and monotonic.
inline float controller_percent(int level) {
    level = std::clamp(level, 1, 100);
    return level <= 50 ? 25.f + (level - 1) * 75.f / 49.f : 100.f + (level - 50) * 4.f;
}

inline int level_from_percent(int percent) {
    int best = 1;
    for (int value = 2; value <= 100; ++value) {
        if (std::abs(controller_percent(value) - percent)
            < std::abs(controller_percent(best) - percent)) {
            best = value;
        }
    }
    return best;
}

inline std::string bar(int level) {
    level = std::clamp(level, 1, 100);
    int fill = (level + 9) / 10;
    return "[" + std::string(fill, '=') + std::string(10 - fill, '.') + "] " + std::to_string(level)
           + "/100";
}

inline int read_level(std::istream& in, int fallback) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("camera_level=", 0) != 0) {
            continue;
        }
        try {
            std::size_t used = 0;
            auto value = line.substr(13);
            int level = std::stoi(value, &used);
            if (used == value.size() && level >= 1 && level <= 100) {
                return level;
            }
        } catch (...) {
        }
    }
    return fallback;
}

// Only controller axes use these multipliers; mouse counts keep their own sensitivity.
inline float controller_rate(float axis, int degrees, float speed, int aim = 100) {
    return rate(axis, degrees) * (speed / 100.f) * (aim / 100.f);
}
}  // namespace modern
