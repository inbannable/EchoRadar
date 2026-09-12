#include "RecognitionModelLocator.h"

#include <array>
#include <system_error>
#include <vector>

namespace EchoRadar {
namespace {

constexpr std::array<const char*, 2> kDefaultPackageNames{{
    "recognition-candidate",
    "v4-candidate",
}};

bool HasRecognitionMetadata(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / "model.json", error);
}

void AddSearchRoot(std::vector<std::filesystem::path>& roots,
                   const std::filesystem::path& root) {
    if (root.empty()) return;
    for (const auto& existing : roots) {
        if (existing == root) return;
    }
    roots.push_back(root);
}

} // namespace

std::filesystem::path FindDefaultRecognitionModelDirectory(
    const std::filesystem::path& executablePath,
    const std::filesystem::path& workingDirectory) {
    const std::filesystem::path executableDirectory =
        executablePath.parent_path();
    std::vector<std::filesystem::path> roots;
    AddSearchRoot(roots, executableDirectory);
    AddSearchRoot(roots, workingDirectory);
    for (auto ancestor = executableDirectory.parent_path();
         !ancestor.empty();) {
        AddSearchRoot(roots, ancestor);
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }

    for (const char* packageName : kDefaultPackageNames) {
        for (const auto& root : roots) {
            const auto candidate = root / "models" / packageName;
            if (!HasRecognitionMetadata(candidate)) continue;
            std::error_code error;
            const auto normalized =
                std::filesystem::weakly_canonical(candidate, error);
            return error ? candidate : normalized;
        }
    }
    return {};
}

} // namespace EchoRadar
