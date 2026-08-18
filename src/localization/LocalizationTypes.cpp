#include "LocalizationTypes.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace EchoRadar {

const LocalizationTuning::PeakWindowTuning& LocalizationTuning::PeakWindowFor(
    SoundClass soundClass) const {
    return soundClass == SoundClass::Gunshot ? gunshotPeak : footstepPeak;
}

std::string AudioProfile::StableKey() const {
    std::ostringstream output;
    output << "profile-v1|" << ToString(eqProfile) << '|'
           << std::fixed << std::setprecision(1)
           << std::clamp(leftRightIsolationPercent, 0.0f, 100.0f) << '|'
           << (perspectiveCorrection ? '1' : '0') << '|'
           << std::setprecision(4) << std::max(displayAspectRatio, 0.1f) << '|'
           << ToString(spatialEnhancement) << '|' << outputEndpointId;
    return output.str();
}

float WrapDirectionDegrees(float angle) {
    if (!std::isfinite(angle)) return 0.0f;
    angle = std::fmod(angle, 360.0f);
    if (angle < 0.0f) angle += 360.0f;
    return angle;
}

float CircularDistanceDegrees(float left, float right) {
    const float difference = std::abs(WrapDirectionDegrees(left) - WrapDirectionDegrees(right));
    return std::min(difference, 360.0f - difference);
}

DirectionResult DirectionSceneResult::LegacyResult(uint64_t eventId) const {
    DirectionResult result;
    result.eventId = eventId;
    result.sceneId = sceneId;
    result.status = status;
    result.inferenceMilliseconds = inferenceMilliseconds;
    if (sourceCount == 0) return result;
    result.primaryAngleDegrees = sources[0].azimuthDegrees;
    result.primaryElevationDegrees = sources[0].elevationDegrees;
    result.confidence = sources[0].confidence;
    result.uncertaintyDegrees = sources[0].uncertaintyDegrees;
    if (sourceCount > 1) {
        result.secondaryAngleDegrees = sources[1].azimuthDegrees;
        result.secondaryElevationDegrees = sources[1].elevationDegrees;
        result.secondaryConfidence = sources[1].confidence;
    }
    return result;
}

} // namespace EchoRadar
