#include "ConfigManager.hpp"
#include <cstdlib>
#include <hyprlang.hpp>
#include <hyprutils/path/Path.hpp>
#include <string>
#include <sys/ucontext.h>
#include "helpers/Log.hpp"
#include <array>
#include <cctype>
#include <string_view>

// Outside the valid range, so "not set" is distinguishable from 0°.
static constexpr Hyprlang::FLOAT LOCATION_UNSET = 1000.F;

static std::string               getMainConfigPath() {
    static const auto paths = Hyprutils::Path::findConfig("hyprsunset");

    return paths.first.value_or("");
}

CConfigManager::CConfigManager(std::string configPath) :
    m_config(configPath.empty() ? getMainConfigPath().c_str() : configPath.c_str(), Hyprlang::SConfigOptions{.throwAllErrors = true, .allowMissingConfig = true}) {
    currentConfigPath = configPath.empty() ? getMainConfigPath() : configPath;
}

void CConfigManager::init() {
    m_config.addConfigValue("max-gamma", Hyprlang::INT{100});
    m_config.addConfigValue("latitude", LOCATION_UNSET);
    m_config.addConfigValue("longitude", LOCATION_UNSET);

    m_config.addSpecialCategory("profile", Hyprlang::SSpecialCategoryOptions{.key = nullptr, .anonymousKeyBased = true});
    m_config.addSpecialConfigValue("profile", "time", Hyprlang::STRING{"00:00"});
    m_config.addSpecialConfigValue("profile", "temperature", Hyprlang::INT{6000});
    m_config.addSpecialConfigValue("profile", "gamma", Hyprlang::FLOAT{1.0f});
    m_config.addSpecialConfigValue("profile", "identity", Hyprlang::INT{0});
    m_config.addSpecialConfigValue("profile", "on-switch", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("profile", "transition", Hyprlang::INT{0});

    m_config.commence();

    auto result = m_config.parse();

    if (result.error)
        Debug::log(ERR, "Config has errors:\n{}\nProceeding ignoring faulty entries", result.getError());
}

// "sunset", "sunrise+30", "dusk-15": a sun event with an offset in minutes.
static std::optional<std::pair<NSun::eEvent, std::chrono::minutes>> parseSunTime(std::string_view text) {
    static constexpr std::array EVENTS = {NSun::EVENT_DAWN, NSun::EVENT_SUNRISE, NSun::EVENT_SUNSET, NSun::EVENT_DUSK};

    for (const auto event : EVENTS) {
        const std::string_view name = NSun::eventName(event);
        if (!text.starts_with(name))
            continue;

        const auto rest = text.substr(name.size());
        if (rest.empty())
            return std::pair{event, std::chrono::minutes::zero()};

        // A sign, then digits only: stoi alone would also take "+-5" or "+ 5".
        if ((rest[0] != '+' && rest[0] != '-') || rest.size() < 2 || !std::isdigit(static_cast<unsigned char>(rest[1])))
            return std::nullopt;

        int minutes = 0;
        try {
            size_t used = 0;
            minutes     = std::stoi(std::string{rest.substr(1)}, &used);
            if (used != rest.size() - 1)
                return std::nullopt;
        } catch (const std::exception&) { return std::nullopt; }

        return std::pair{event, std::chrono::minutes(rest[0] == '-' ? -minutes : minutes)};
    }

    return std::nullopt;
}

std::vector<SSunsetProfile> CConfigManager::getSunsetProfiles() {
    std::vector<SSunsetProfile> result;

    auto                        keys = m_config.listKeysForSpecialCategory("profile");
    result.reserve(keys.size());

    for (auto& key : keys) {
        std::string   time;
        unsigned long temperature;
        float         gamma;
        bool          identity;
        std::string   onSwitch;
        Hyprlang::INT transition = 0;

        try {
            time        = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("profile", "time", key.c_str()));
            temperature = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("profile", "temperature", key.c_str()));
            gamma       = std::any_cast<Hyprlang::FLOAT>(m_config.getSpecialConfigValue("profile", "gamma", key.c_str()));
            identity    = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("profile", "identity", key.c_str()));
            onSwitch    = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("profile", "on-switch", key.c_str()));
            transition  = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("profile", "transition", key.c_str()));
        } catch (const std::bad_any_cast& e) {
            RASSERT(false, "Failed to construct Profile: {}", e.what()); //
        } catch (const std::out_of_range& e) {
            RASSERT(false, "Missing property for Profile: {}", e.what()); //
        }

        int  hour = 0, minute = 0;
        auto sunTime = parseSunTime(time);

        if (!sunTime) {
            size_t separator = time.find(':');

            if (separator == std::string::npos) {
                Debug::log(ERR, "Invalid time format: {}, skipping profile {}", time, key);
                continue;
            }

            try {
                hour   = std::stoi(time.substr(0, separator));
                minute = std::stoi(time.substr(separator + 1).c_str());
            } catch (const std::exception& e) {
                Debug::log(ERR, "Invalid time format: {}, skipping profile {}", time, key);
                continue;
            }
        }

        if (transition < 0) {
            Debug::log(ERR, "Invalid transition: {} minutes, using 0 for profile {}", transition, key);
            transition = 0;
        }

        // clang-format off
        result.push_back(SSunsetProfile{
            .time = {
                .hour   = std::chrono::hours(hour),
                .minute = std::chrono::minutes(minute),
            },
            .temperature = temperature,
            .gamma       = gamma,
            .identity    = identity,
            .onSwitch    = onSwitch,
            .transition  = std::chrono::minutes(transition),
            .sunEvent    = sunTime ? sunTime->first : NSun::EVENT_NONE,
            .sunOffset   = sunTime ? sunTime->second : std::chrono::minutes::zero(),
            .id          = result.size(),
        });
        // clang-format on
    }

    return result;
}

std::optional<NSun::SLocation> CConfigManager::getLocation() {
    Hyprlang::FLOAT latitude = LOCATION_UNSET, longitude = LOCATION_UNSET;
    try {
        latitude  = std::any_cast<Hyprlang::FLOAT>(m_config.getConfigValue("latitude"));
        longitude = std::any_cast<Hyprlang::FLOAT>(m_config.getConfigValue("longitude"));
    } catch (const std::bad_any_cast& e) {
        RASSERT(false, "Failed to construct location: {}", e.what()); //
    }

    if (latitude == LOCATION_UNSET && longitude == LOCATION_UNSET)
        return std::nullopt;

    if (latitude == LOCATION_UNSET || longitude == LOCATION_UNSET) {
        Debug::log(ERR, "Set both latitude and longitude to follow the sun");
        return std::nullopt;
    }

    if (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) {
        Debug::log(ERR, "Invalid location {}, {}: latitude must be within -90..90 and longitude within -180..180", latitude, longitude);
        return std::nullopt;
    }

    return NSun::SLocation{.latitude = latitude, .longitude = longitude};
}

float CConfigManager::getMaxGamma() {
    try {
        return std::any_cast<Hyprlang::INT>(m_config.getConfigValue("max-gamma")) / 100.f;
    } catch (const std::bad_any_cast& e) {
        RASSERT(false, "Failed to construct max-gamma: {}", e.what()); //
    }
}
