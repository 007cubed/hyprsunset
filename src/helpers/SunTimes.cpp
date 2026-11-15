#include "SunTimes.hpp"
#include <cmath>
#include <numbers>

using namespace NSun;

// Written from the equations in NOAA's Solar Calculator "Calculation Details"
// (https://gml.noaa.gov/grad/solcalc/calcdetails.html), after Meeus, Astronomical Algorithms.
// NOAA's material is a US government work in the public domain; no code was copied.

constexpr double UNIX_EPOCH_JULIAN_DAY = 2440587.5;
constexpr double J2000_JULIAN_DAY      = 2451545.0;
constexpr double DAYS_PER_CENTURY      = 36525.0;
// Refraction and the solar disc: the sun rises when its centre is 0.833° below the horizon.
constexpr double SUNRISE_ZENITH = 90.833;

static double    rad(double deg) {
    return deg * std::numbers::pi / 180.0;
}

static double deg(double rad) {
    return rad * 180.0 / std::numbers::pi;
}

SSunTimes NSun::calculate(std::chrono::sys_days date, double latitude, double longitude) {
    // Julian centuries since J2000 at local solar noon, close enough for both events.
    const double julianDay = date.time_since_epoch().count() + UNIX_EPOCH_JULIAN_DAY + 0.5 - longitude / 360.0;
    const double t         = (julianDay - J2000_JULIAN_DAY) / DAYS_PER_CENTURY;

    const double meanLongitude = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double meanAnomaly   = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double eccentricity  = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);

    const double centre = std::sin(rad(meanAnomaly)) * (1.914602 - t * (0.004817 + 0.000014 * t)) + std::sin(rad(2 * meanAnomaly)) * (0.019993 - 0.000101 * t) +
        std::sin(rad(3 * meanAnomaly)) * 0.000289;
    const double omega             = 125.04 - 1934.136 * t;
    const double apparentLongitude = meanLongitude + centre - 0.00569 - 0.00478 * std::sin(rad(omega));
    const double meanObliquity     = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double obliquity         = meanObliquity + 0.00256 * std::cos(rad(omega));
    const double declination       = std::asin(std::sin(rad(obliquity)) * std::sin(rad(apparentLongitude)));

    const double y                  = std::pow(std::tan(rad(obliquity / 2)), 2);
    const double equationOfTimeMins = 4 *
        deg(y * std::sin(2 * rad(meanLongitude)) - 2 * eccentricity * std::sin(rad(meanAnomaly)) +
            4 * eccentricity * y * std::sin(rad(meanAnomaly)) * std::cos(2 * rad(meanLongitude)) - 0.5 * y * y * std::sin(4 * rad(meanLongitude)) -
            1.25 * eccentricity * eccentricity * std::sin(2 * rad(meanAnomaly)));

    // Outside [-1, 1] the sun stays below (polar night) or above (polar day) the horizon all day.
    const double cosHourAngle = std::cos(rad(SUNRISE_ZENITH)) / (std::cos(rad(latitude)) * std::cos(declination)) - std::tan(rad(latitude)) * std::tan(declination);
    if (std::abs(cosHourAngle) > 1.0)
        return {};

    // Minutes after 00:00 UTC on date; not wrapped, so far east or west the instant lands on the neighbouring UTC day.
    const double noonMins    = 720.0 - 4.0 * longitude - equationOfTimeMins;
    const double halfDayMins = 4.0 * deg(std::acos(cosHourAngle));
    const auto   at          = [date](double minutes) { return std::chrono::round<std::chrono::seconds>(date + std::chrono::duration<double, std::ratio<60>>(minutes)); };

    return {.sunrise = at(noonMins - halfDayMins), .sunset = at(noonMins + halfDayMins)};
}
