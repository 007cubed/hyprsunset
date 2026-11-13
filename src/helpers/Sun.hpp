#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Sunrise, sunset and civil twilight from NOAA's "General Solar Position Calculations"
// (public domain). Accurate to about a minute away from the poles.
namespace NSun {
    enum eEvent : uint8_t {
        EVENT_NONE = 0,
        EVENT_DAWN,    // civil twilight begins, sun 6° below the horizon
        EVENT_SUNRISE, // upper limb on the horizon, with refraction
        EVENT_SUNSET,
        EVENT_DUSK, // civil twilight ends
    };

    struct SLocation {
        double latitude  = 0.0; // degrees, north positive
        double longitude = 0.0; // degrees, east positive
    };

    // When the event happens on the given calendar day (UTC date), or nullopt if the
    // sun does not cross that altitude that day (polar day or night).
    std::optional<std::chrono::sys_seconds> eventTime(eEvent event, std::chrono::sys_days day, const SLocation& location);

    const char*                             eventName(eEvent event);
}
