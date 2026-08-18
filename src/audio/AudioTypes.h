#pragma once

#include <cstdint>
#include <string>

namespace EchoRadar {

struct AudioLevels {
    float leftRms{0.0f};
    float rightRms{0.0f};
    float leftPeak{0.0f};
    float rightPeak{0.0f};
};

enum class HeadphoneEqProfile : uint8_t {
    Natural,
    Crisp,
    Smooth,
};

enum class SpatialEnhancementState : uint8_t {
    Off,
    On,
    Unknown,
};

inline const char* ToString(HeadphoneEqProfile profile) {
    switch (profile) {
    case HeadphoneEqProfile::Natural: return "natural";
    case HeadphoneEqProfile::Crisp: return "crisp";
    case HeadphoneEqProfile::Smooth: return "smooth";
    }
    return "natural";
}

inline const char* ToString(SpatialEnhancementState state) {
    switch (state) {
    case SpatialEnhancementState::Off: return "off";
    case SpatialEnhancementState::On: return "on";
    case SpatialEnhancementState::Unknown: return "unknown";
    }
    return "unknown";
}

struct AudioProfile {
    std::string name{"Default"};
    HeadphoneEqProfile eqProfile{HeadphoneEqProfile::Natural};
    float leftRightIsolationPercent{0.0f};
    bool perspectiveCorrection{true};
    float displayAspectRatio{16.0f / 9.0f};
    SpatialEnhancementState spatialEnhancement{SpatialEnhancementState::Unknown};
    std::string outputEndpointId;
};

} // namespace EchoRadar
