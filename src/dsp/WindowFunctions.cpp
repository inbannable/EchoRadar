#include "WindowFunctions.h"
#include <cmath>

namespace EchoRadar {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

std::vector<float> MakeHannWindow(size_t size) {
    std::vector<float> window(size, 1.0f);
    if (size <= 1) {
        if (size == 1) {
            window[0] = 1.0f;
        }
        return window;
    }

    const float denom = static_cast<float>(size - 1);
    for (size_t i = 0; i < size; ++i) {
        const float phase = 2.0f * kPi * static_cast<float>(i) / denom;
        window[i] = 0.5f * (1.0f - std::cos(phase));
    }
    return window;
}

} // namespace EchoRadar
