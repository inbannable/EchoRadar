#pragma once

#include <filesystem>

namespace EchoRadar {

/// Finds the default recognition package next to the executable, below the
/// working directory, or below an executable ancestor. The current package
/// name is preferred, with the pre-v2 v4-candidate name retained so existing
/// installations keep event radar enabled after upgrading.
std::filesystem::path FindDefaultRecognitionModelDirectory(
    const std::filesystem::path& executablePath,
    const std::filesystem::path& workingDirectory = {});

} // namespace EchoRadar
