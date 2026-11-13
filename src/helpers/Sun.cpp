#include "Sun.hpp"
#include <cmath>
#include <numbers>

using namespace NSun;

static double toRadians(double degrees) {
    return degrees * std::numbers::pi / 180.0;
}

static double toDegrees(double radians) {
    return radians * 180.0 / std::numbers::pi;
}

std::optional<std::chrono::sys_seconds> NSun::eventTime(eEvent event, std::chrono::sys_days day, const SLocation& location) {
    // Zenith angle of the sun's centre at the event: 90° plus refraction and solar radius, or 96° for civil twilight.
    double zenith = 0.0;
    switch (event) {
        case EVENT_SUNRISE:
        case EVENT_SUNSET: zenith = 90.833; break;
        case EVENT_DAWN:
        case EVENT_DUSK: zenith = 96.0; break;
        case EVENT_NONE: return std::nullopt;
    }

    const std::chrono::year_month_day date{day};
    const auto                        dayOfYear  = (day - std::chrono::sys_days{date.year() / std::chrono::January / 1}).count() + 1;
    const auto                        daysInYear = date.year().is_leap() ? 366.0 : 365.0;

    // Fractional year at noon, in radians.
    const double gamma = 2.0 * std::numbers::pi / daysInYear * (dayOfYear - 1);

    // Equation of time in minutes, and solar declination in radians.
    const double eqTime      = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) - 0.014615 * std::cos(2 * gamma) - 0.040849 * std::sin(2 * gamma));
    const double declination = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) - 0.006758 * std::cos(2 * gamma) + 0.000907 * std::sin(2 * gamma) -
        0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);

    const double latitude     = toRadians(location.latitude);
    const double cosHourAngle = std::cos(toRadians(zenith)) / (std::cos(latitude) * std::cos(declination)) - std::tan(latitude) * std::tan(declination);

    // Outside [-1, 1] the sun stays above (midnight sun) or below (polar night) that altitude all day.
    if (!std::isfinite(cosHourAngle) || cosHourAngle < -1.0 || cosHourAngle > 1.0)
        return std::nullopt;

    const double hourAngle = toDegrees(std::acos(cosHourAngle));
    const bool   morning   = event == EVENT_DAWN || event == EVENT_SUNRISE;

    // Minutes after 00:00 UTC. Four minutes per degree of longitude or hour angle.
    const double minutes = 720.0 - 4.0 * (location.longitude + (morning ? hourAngle : -hourAngle)) - eqTime;

    return std::chrono::time_point_cast<std::chrono::seconds>(day + std::chrono::duration<double, std::ratio<60>>(minutes));
}

const char* NSun::eventName(eEvent event) {
    switch (event) {
        case EVENT_DAWN: return "dawn";
        case EVENT_SUNRISE: return "sunrise";
        case EVENT_SUNSET: return "sunset";
        case EVENT_DUSK: return "dusk";
        case EVENT_NONE: break;
    }
    return "";
}
