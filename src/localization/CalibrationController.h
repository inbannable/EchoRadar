#pragma once

#include "StereoDirectionEstimator.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace EchoRadar {

class CalibrationController {
public:
    enum class Mode : uint8_t {
        Quick,
        Full,
    };

    struct State {
        bool active{false};
        bool armed{false};
        bool complete{false};
        Mode mode{Mode::Quick};
        float targetAngleDegrees{0.0f};
        size_t acceptedSamples{0};
        size_t requiredSamples{0};
        size_t distinctBearings{0};
        std::string lastMessage;
    };

    explicit CalibrationController(std::filesystem::path path = {});

    void Begin(Mode mode, const AudioProfile& profile);
    void Cancel();
    void ArmNext();
    bool AcceptArmedSample(SoundClass soundClass,
                           const StereoDirectionFeatures& features);
    State Snapshot() const;
    DirectionCalibrationProfile ProfileSnapshot() const;

    bool Load(std::string* error = nullptr);
    bool Save(std::string* error = nullptr) const;
    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
    mutable std::mutex m_mutex;
    State m_state;
    std::vector<float> m_targets;
    size_t m_targetIndex{0};
    DirectionCalibrationProfile m_profile;
};

} // namespace EchoRadar
