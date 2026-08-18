#pragma once

#include <filesystem>
#include <string>

namespace EchoRadar {

std::string ComputeFileSha256(const std::filesystem::path& path,
                              bool* ok = nullptr);

} // namespace EchoRadar
