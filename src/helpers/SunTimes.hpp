#pragma once

#include <chrono>
#include <optional>

namespace NSun {
    struct SSunTimes {
        // Empty when the sun does not rise or set that day (polar day or night).
        std::optional<std::chrono::sys_seconds> sunrise;
        std::optional<std::chrono::sys_seconds> sunset;
    };

    // Sunrise and sunset around solar noon of the UTC date `date` at a location, in degrees (north and
    // east positive). Pure: reads no clock or time zone.
    SSunTimes calculate(std::chrono::sys_days date, double latitude, double longitude);
}
