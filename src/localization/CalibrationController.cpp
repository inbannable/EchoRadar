#include "CalibrationController.h"

#include <algorithm>
#include <cmath>

namespace EchoRadar {

CalibrationController::CalibrationController(std::filesystem::path path)
    : m_path(std::move(path)) {
    if (m_path.empty()) {
        m_path = std::filesystem::current_path() / ".echoradar" /
            "direction-calibration.tsv";
    }
}

void CalibrationController::Begin(Mode mode, const AudioProfile& profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = {};
    m_state.active = true;
    m_state.mode = mode;
    m_state.lastMessage = "Face the fixed sound source at the displayed bearing, then arm capture.";
    m_targets.clear();
    const int step = mode == Mode::Quick ? 45 : 15;
    const int repetitions = mode == Mode::Quick ? 3 : 4;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (int angle = 0; angle < 360; angle += step) {
            m_targets.push_back(static_cast<float>(angle));
        }
    }
    m_targetIndex = 0;
    m_state.requiredSamples = m_targets.size();
    m_state.distinctBearings = static_cast<size_t>(360 / step);
    m_state.targetAngleDegrees = m_targets.front();
    m_profile.Clear();
    m_profile.SetAudioProfileKey(profile.StableKey());
}

void CalibrationController::Cancel() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.active = false;
    m_state.armed = false;
    m_state.lastMessage = "Calibration stopped.";
}

void CalibrationController::ArmNext() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_state.active || m_state.complete || m_targets.empty()) return;
    m_state.armed = true;
    m_state.lastMessage = "Armed: waiting for the next accepted remote event.";
}

bool CalibrationController::AcceptArmedSample(
    SoundClass soundClass,
    const StereoDirectionFeatures& features) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_state.active || !m_state.armed || m_state.complete ||
        m_targetIndex >= m_targets.size()) return false;
    float coherence = 0.0f;
    for (float value : features.bandCoherence) coherence += value;
    coherence /= static_cast<float>(features.bandCoherence.size());
    const float gccQuality = std::sqrt(
        std::max(0.0f, features.gccSharpness * features.gccPeakToSidelobe));
    if (features.schemaVersion != StereoDirectionFeatures::kSchemaVersion ||
        features.rms < 1.0e-5f || features.peakToNoiseDb < 6.0f ||
        features.activeFrameFraction < 0.015f || gccQuality < 0.08f ||
        coherence < 0.15f || features.stereoQuality < 0.25f) {
        m_state.armed = false;
        m_state.lastMessage = "Sample rejected: peak, GCC, or coherence quality was too low. Re-arm to retry.";
        return false;
    }
    m_profile.AddSample({soundClass, m_targets[m_targetIndex], features});
    ++m_targetIndex;
    m_state.acceptedSamples = m_targetIndex;
    m_state.armed = false;
    if (m_targetIndex >= m_targets.size()) {
        m_state.complete = true;
        m_state.active = false;
        m_state.lastMessage = "Calibration complete and ready to save.";
    } else {
        m_state.targetAngleDegrees = m_targets[m_targetIndex];
        m_state.lastMessage = "Sample accepted. Move to the next displayed bearing.";
    }
    return true;
}

CalibrationController::State CalibrationController::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

DirectionCalibrationProfile CalibrationController::ProfileSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_profile;
}

bool CalibrationController::Load(std::string* error) {
    DirectionCalibrationProfile loaded;
    if (!loaded.Load(m_path, error)) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_profile = std::move(loaded);
    m_state.acceptedSamples = m_profile.SampleCount();
    m_state.lastMessage = "Saved calibration loaded.";
    return true;
}

bool CalibrationController::Save(std::string* error) const {
    return ProfileSnapshot().Save(m_path, error);
}

} // namespace EchoRadar
