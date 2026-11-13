#pragma once

#include "Hyprsunset.hpp"
#include <hyprlang.hpp>
#include <optional>
#include <vector>

class CConfigManager {
  public:
    CConfigManager(std::string configPath);

    std::vector<SSunsetProfile> getSunsetProfiles();
    float                       getMaxGamma();
    // nullopt unless both latitude and longitude are set.
    std::optional<NSun::SLocation> getLocation();

    void                           init();

  private:
    Hyprlang::CConfig m_config;

    std::string       currentConfigPath;
};

inline UP<CConfigManager> g_pConfigManager;
