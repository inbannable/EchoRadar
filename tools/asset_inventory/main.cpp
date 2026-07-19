#include "dataset/AssetInventory.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using EchoRadar::AssetInventory;

namespace {

void PrintUsage() {
    std::cout
        << "Usage: asset_inventory --asset-root <path> [options]\n\n"
        << "Options:\n"
        << "  --asset-root <path>         Root containing extracted WAV assets\n"
        << "  --output-dir <path>         Report directory (default: sibling asset-inventory)\n"
        << "  --help                      Show this help message\n\n"
        << "The source tree is read-only. Reports written to the output directory:\n"
        << "  asset_manifest.csv, review_needed.csv, summary.json\n";
}

} // namespace

int main(int argc, char** argv) {
    fs::path assetRoot;
    fs::path outputDirectory;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        }
        if (arg == "--asset-root") {
            if (i + 1 >= argc) {
                std::cerr << "[Error] --asset-root requires a path\n";
                return 2;
            }
            assetRoot = argv[++i];
            continue;
        }
        if (arg == "--output-dir") {
            if (i + 1 >= argc) {
                std::cerr << "[Error] --output-dir requires a path\n";
                return 2;
            }
            outputDirectory = argv[++i];
            continue;
        }
        std::cerr << "[Error] Unknown option: " << arg << '\n';
        PrintUsage();
        return 2;
    }

    if (assetRoot.empty()) {
        std::cerr << "[Error] --asset-root is required\n";
        PrintUsage();
        return 2;
    }
    if (outputDirectory.empty()) {
        outputDirectory = assetRoot.parent_path() / "asset-inventory";
    }

    AssetInventory inventory(assetRoot);
    std::string error;
    if (!inventory.Scan(&error)) {
        std::cerr << "[Error] " << error << '\n';
        return 1;
    }
    if (!inventory.Export(outputDirectory, &error)) {
        std::cerr << "[Error] " << error << '\n';
        return 1;
    }

    const auto& summary = inventory.GetSummary();
    std::cout
        << "Asset inventory complete\n"
        << "  Source:       " << fs::absolute(assetRoot).string() << '\n'
        << "  Reports:      " << fs::absolute(outputDirectory).string() << '\n'
        << "  Valid WAV:    " << summary.validWavFiles << '\n'
        << "  Ignored:      " << summary.ignoredFiles << '\n'
        << "  Invalid WAV:  " << summary.invalidWavFiles << '\n'
        << "  Duplicates:   " << summary.duplicateFiles << '\n'
        << "  Review needed:" << summary.reviewNeeded << '\n'
        << "  Gunshot:      " << summary.gunshots << '\n'
        << "  Footstep:     " << summary.footsteps << '\n'
        << "  Mechanical:   " << summary.mechanical << '\n'
        << "  Other:        " << summary.other << '\n';

    return summary.invalidWavFiles == 0 ? 0 : 3;
}
