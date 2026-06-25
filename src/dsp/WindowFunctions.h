#pragma once
#include <cstddef>
#include <vector>

namespace EchoRadar {

/// Returns a periodic-symmetric Hann window of size N.
/// Formula: w[n] = 0.5 * (1 - cos(2*pi*n/(N-1))) for n in [0, N-1].
std::vector<float> MakeHannWindow(size_t size);

} // namespace EchoRadar
