#include "dataset/AssetInventory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace EchoRadar {
namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool Contains(const std::string& value, const std::string& part) {
    return value.find(part) != std::string::npos;
}

std::string CsvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string escaped{"\""};
    for (const char c : value) {
        if (c == '\"') escaped += '\"';
        escaped += c;
    }
    escaped += '\"';
    return escaped;
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '\"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

uint16_t ReadLe16(const unsigned char* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
}

uint32_t ReadLe32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) |
           (static_cast<uint32_t>(bytes[3]) << 24u);
}

uint32_t RotateRight(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32u - count));
}

class Sha256 {
public:
    void Update(const unsigned char* data, size_t size) {
        m_totalBytes += size;
        while (size > 0) {
            const size_t take = std::min(size, m_buffer.size() - m_bufferSize);
            std::memcpy(m_buffer.data() + m_bufferSize, data, take);
            m_bufferSize += take;
            data += take;
            size -= take;
            if (m_bufferSize == m_buffer.size()) {
                Transform(m_buffer.data());
                m_bufferSize = 0;
            }
        }
    }

    std::string Finish() {
        const uint64_t bitLength = static_cast<uint64_t>(m_totalBytes) * 8ull;
        m_buffer[m_bufferSize++] = 0x80u;
        if (m_bufferSize > 56) {
            std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_bufferSize), m_buffer.end(), 0u);
            Transform(m_buffer.data());
            m_bufferSize = 0;
        }
        std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_bufferSize), m_buffer.begin() + 56, 0u);
        for (size_t i = 0; i < 8; ++i) {
            m_buffer[63 - i] = static_cast<unsigned char>((bitLength >> (i * 8u)) & 0xffu);
        }
        Transform(m_buffer.data());

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (const uint32_t value : m_state) out << std::setw(8) << value;
        return out.str();
    }

private:
    void Transform(const unsigned char* block) {
        static constexpr std::array<uint32_t, 64> constants = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t offset = i * 4;
            words[i] = (static_cast<uint32_t>(block[offset]) << 24u) |
                       (static_cast<uint32_t>(block[offset + 1]) << 16u) |
                       (static_cast<uint32_t>(block[offset + 2]) << 8u) |
                       static_cast<uint32_t>(block[offset + 3]);
        }
        for (size_t i = 16; i < words.size(); ++i) {
            const uint32_t s0 = RotateRight(words[i - 15], 7u) ^ RotateRight(words[i - 15], 18u) ^ (words[i - 15] >> 3u);
            const uint32_t s1 = RotateRight(words[i - 2], 17u) ^ RotateRight(words[i - 2], 19u) ^ (words[i - 2] >> 10u);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = m_state[0];
        uint32_t b = m_state[1];
        uint32_t c = m_state[2];
        uint32_t d = m_state[3];
        uint32_t e = m_state[4];
        uint32_t f = m_state[5];
        uint32_t g = m_state[6];
        uint32_t h = m_state[7];

        for (size_t i = 0; i < words.size(); ++i) {
            const uint32_t sum1 = RotateRight(e, 6u) ^ RotateRight(e, 11u) ^ RotateRight(e, 25u);
            const uint32_t choose = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
            const uint32_t sum0 = RotateRight(a, 2u) ^ RotateRight(a, 13u) ^ RotateRight(a, 22u);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<uint32_t, 8> m_state{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<unsigned char, 64> m_buffer{};
    size_t m_bufferSize{0};
    size_t m_totalBytes{0};
};

struct Classification {
    AssetLabel label{AssetLabel::Other};
    std::string subtype;
    std::string weapon;
    std::string surface;
    std::string distance;
    std::string sourceGroup;
    std::string rule;
    float confidence{0.0f};
};

const std::set<std::string>& FirearmFolders() {
    static const std::set<std::string> names = {
        "ak47", "aug", "awp", "bizon", "cz75a", "deagle", "elite", "famas", "fiveseven",
        "g3sg1", "galilar", "glock18", "hkp2000", "m249", "m4a1", "mac10", "mag7", "mp5",
        "mp7", "mp9", "negev", "nova", "p250", "p90", "revolver", "sawedoff", "scar20",
        "sg556", "ssg08", "tec9", "ump45", "usp", "xm1014",
    };
    return names;
}

std::string FootstepSurface(std::string stem) {
    const size_t lastUnderscore = stem.find_last_of('_');
    if (lastUnderscore != std::string::npos && lastUnderscore + 1 < stem.size()) {
        const std::string suffix = stem.substr(lastUnderscore + 1);
        if (std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            stem.erase(lastUnderscore);
        }
    }
    if (stem.size() > 3 && stem.ends_with("_ct")) stem.resize(stem.size() - 3);
    else if (stem.size() > 2 && stem.ends_with("_t")) stem.resize(stem.size() - 2);
    return stem;
}

std::string MechanicalSubtype(const std::string& stem) {
    if (Contains(stem, "clip") || Contains(stem, "reload") || Contains(stem, "box") ||
        Contains(stem, "chain") || Contains(stem, "cover") || Contains(stem, "insertshell")) return "reload";
    if (Contains(stem, "bolt") || Contains(stem, "slide") || Contains(stem, "pump") ||
        Contains(stem, "hammer") || Contains(stem, "prepare") || Contains(stem, "sideback") ||
        Contains(stem, "siderelease") || Contains(stem, "mech")) return "weapon_action";
    if (Contains(stem, "silencer_on") || Contains(stem, "silencer_off") || Contains(stem, "silencer_screw")) return "attachment";
    if (Contains(stem, "draw") || Contains(stem, "deploy")) return "draw";
    if (Contains(stem, "zoom")) return "zoom";
    if (Contains(stem, "switch")) return "switch";
    if (Contains(stem, "pinpull") || Contains(stem, "throw")) return "grenade_handling";
    if (Contains(stem, "plant") || Contains(stem, "disarm") || Contains(stem, "key_press") ||
        Contains(stem, "initiate") || Contains(stem, "click")) return "device_handling";
    if (Contains(stem, "movement") || Contains(stem, "lowammo")) return "handling";
    if (Contains(stem, "special") || Contains(stem, "taunt") || Contains(stem, "element")) return "handling";
    return {};
}

Classification Classify(const fs::path& relativePath) {
    std::vector<std::string> parts;
    for (const auto& part : relativePath) parts.push_back(Lower(part.string()));
    const std::string stem = Lower(relativePath.stem().string());

    if (parts.size() >= 3 && parts[0] == "player" && parts[1] == "footsteps") {
        Classification result;
        result.label = AssetLabel::Footstep;
        result.surface = FootstepSurface(stem);
        result.subtype = StartsWith(result.surface, "land_") ? "landing" : "step";
        result.sourceGroup = "surface:" + result.surface;
        result.rule = "path:player/footsteps";
        result.confidence = 1.0f;
        return result;
    }

    if (parts.size() >= 3 && parts[0] == "weapons" && FirearmFolders().contains(parts[1])) {
        Classification result;
        result.weapon = parts[1];
        result.sourceGroup = "weapon:" + result.weapon;
        const std::string mechanicalSubtype = MechanicalSubtype(stem);
        if (!mechanicalSubtype.empty()) {
            result.label = AssetLabel::Mechanical;
            result.subtype = mechanicalSubtype;
            result.rule = "firearm-folder:mechanical-token";
            result.confidence = 0.98f;
            return result;
        }

        result.label = AssetLabel::Gunshot;
        result.subtype = Contains(stem, "silencer") || Contains(stem, "_us_") || Contains(stem, "unsilenced")
            ? "weapon_report_variant" : "weapon_report";
        result.distance = Contains(stem, "distant") ? "distant" : "near";
        result.rule = "firearm-folder:non-mechanical-report";
        result.confidence = Contains(stem, "clean") ? 0.65f : 0.96f;
        return result;
    }

    const std::string mechanicalSubtype = MechanicalSubtype(stem);
    if (!mechanicalSubtype.empty()) {
        Classification result;
        result.label = AssetLabel::Mechanical;
        result.subtype = mechanicalSubtype;
        if (parts.size() >= 2 && parts[0] == "weapons") result.weapon = parts[1];
        result.sourceGroup = result.weapon.empty() ? "mechanical:misc" : "weapon:" + result.weapon;
        result.rule = "filename:mechanical-token";
        result.confidence = 0.92f;
        return result;
    }

    Classification result;
    result.label = AssetLabel::Other;
    result.confidence = 0.90f;
    result.rule = "fallback:non-target";
    if (!parts.empty() && parts[0] == "player") {
        result.subtype = "player";
        result.sourceGroup = "other:player";
    } else if (parts.size() >= 2 && parts[0] == "weapons" && parts[1] == "fx") {
        result.subtype = "projectile_fx";
        result.sourceGroup = parts.size() >= 3 ? "other:weapon_fx/" + parts[2] : "other:weapon_fx";
    } else if (parts.size() >= 2 && parts[0] == "weapons") {
        result.weapon = parts[1];
        result.subtype = StartsWith(parts[1], "knife") || parts[1] == "bknife" ? "melee" : "weapon_other";
        result.sourceGroup = "other:weapon/" + parts[1];
    } else {
        result.subtype = "other";
        result.sourceGroup = parts.empty() ? "other:misc" : "other:" + parts[0];
    }
    return result;
}

bool IsIgnoredPath(const fs::path& relativePath) {
    for (const auto& part : relativePath) {
        const std::string name = part.string();
        if (name == ".DS_Store" || StartsWith(name, "._")) return true;
    }
    return Lower(relativePath.extension().string()) != ".wav";
}

bool NeedsReview(const AssetRecord& record) {
    return !record.included || record.classificationConfidence < 0.80f;
}

bool WriteCsv(const fs::path& path, const std::vector<AssetRecord>& records, bool reviewOnly, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "Could not open output: " + path.string();
        return false;
    }
    out << "asset_id,relative_path,label,subtype,weapon,surface,distance,source_group,classification_rule,"
           "classification_confidence,sample_rate,channels,bit_depth,frame_count,duration_ms,peak,rms,sha256,"
           "duplicate_of,included,error\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& record : records) {
        if (reviewOnly && !NeedsReview(record)) continue;
        out << CsvEscape(record.assetId) << ','
            << CsvEscape(record.relativePath) << ','
            << ToString(record.label) << ','
            << CsvEscape(record.subtype) << ','
            << CsvEscape(record.weapon) << ','
            << CsvEscape(record.surface) << ','
            << CsvEscape(record.distance) << ','
            << CsvEscape(record.sourceGroup) << ','
            << CsvEscape(record.classificationRule) << ','
            << record.classificationConfidence << ','
            << record.audio.sampleRate << ','
            << record.audio.channels << ','
            << record.audio.bitDepth << ','
            << record.audio.frameCount << ','
            << record.audio.durationMs << ','
            << record.audio.peak << ','
            << record.audio.rms << ','
            << record.sha256 << ','
            << CsvEscape(record.duplicateOf) << ','
            << (record.included ? "true" : "false") << ','
            << CsvEscape(record.audio.error) << '\n';
    }
    if (!out.good()) {
        if (error) *error = "Failed while writing output: " + path.string();
        return false;
    }
    return true;
}

} // namespace

const char* ToString(AssetLabel label) {
    switch (label) {
    case AssetLabel::Gunshot: return "gunshot";
    case AssetLabel::Footstep: return "footstep";
    case AssetLabel::Mechanical: return "mechanical";
    case AssetLabel::Other: return "other";
    }
    return "other";
}

AssetInventory::AssetInventory(fs::path assetRoot)
    : m_assetRoot(std::move(assetRoot)) {}

AssetAudioInfo AssetInventory::ReadPcmWavInfo(const fs::path& path) {
    AssetAudioInfo info;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        info.error = "could not open file";
        return info;
    }

    std::array<unsigned char, 12> header{};
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!in || std::memcmp(header.data(), "RIFF", 4) != 0 || std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
        info.error = "not a RIFF/WAVE file";
        return info;
    }

    bool haveFormat = false;
    bool haveData = false;
    uint16_t audioFormat = 0;
    uint16_t blockAlign = 0;
    uint32_t dataBytes = 0;
    std::streampos dataPosition{};

    while (in) {
        std::array<unsigned char, 8> chunkHeader{};
        in.read(reinterpret_cast<char*>(chunkHeader.data()), static_cast<std::streamsize>(chunkHeader.size()));
        if (!in) break;
        const uint32_t chunkSize = ReadLe32(chunkHeader.data() + 4);
        const std::streampos chunkData = in.tellg();

        if (std::memcmp(chunkHeader.data(), "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                info.error = "invalid fmt chunk";
                return info;
            }
            std::array<unsigned char, 16> format{};
            in.read(reinterpret_cast<char*>(format.data()), static_cast<std::streamsize>(format.size()));
            if (!in) {
                info.error = "truncated fmt chunk";
                return info;
            }
            audioFormat = ReadLe16(format.data());
            info.channels = ReadLe16(format.data() + 2);
            info.sampleRate = ReadLe32(format.data() + 4);
            blockAlign = ReadLe16(format.data() + 12);
            info.bitDepth = ReadLe16(format.data() + 14);
            haveFormat = true;
        } else if (std::memcmp(chunkHeader.data(), "data", 4) == 0) {
            dataBytes = chunkSize;
            dataPosition = chunkData;
            haveData = true;
        }

        const std::streamoff paddedSize = static_cast<std::streamoff>(chunkSize) + static_cast<std::streamoff>(chunkSize & 1u);
        in.clear();
        in.seekg(chunkData + paddedSize);
    }

    if (!haveFormat || !haveData) {
        info.error = "missing fmt or data chunk";
        return info;
    }
    if (audioFormat != 1) {
        info.error = "unsupported WAV encoding (only PCM is supported)";
        return info;
    }
    if (info.channels == 0 || info.sampleRate == 0 || (info.bitDepth != 8 && info.bitDepth != 16)) {
        info.error = "unsupported or invalid PCM format";
        return info;
    }
    const uint16_t expectedAlign = static_cast<uint16_t>(info.channels * (info.bitDepth / 8u));
    if (blockAlign != expectedAlign || blockAlign == 0 || dataBytes == 0) {
        info.error = "invalid PCM block alignment or empty data";
        return info;
    }

    std::vector<unsigned char> data(dataBytes);
    in.clear();
    in.seekg(dataPosition);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (in.gcount() != static_cast<std::streamsize>(data.size())) {
        info.error = "truncated PCM data";
        return info;
    }

    const size_t bytesPerSample = info.bitDepth / 8u;
    const size_t sampleCount = data.size() / bytesPerSample;
    double sumSquares = 0.0;
    float peak = 0.0f;
    for (size_t i = 0; i < sampleCount; ++i) {
        float sample = 0.0f;
        if (info.bitDepth == 8) {
            sample = static_cast<float>(static_cast<int>(data[i]) - 128) / 128.0f;
        } else {
            const uint16_t packed = ReadLe16(data.data() + i * 2u);
            const int16_t signedSample = std::bit_cast<int16_t>(packed);
            sample = static_cast<float>(signedSample) / 32768.0f;
        }
        peak = std::max(peak, std::fabs(sample));
        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }

    info.frameCount = dataBytes / blockAlign;
    info.durationMs = static_cast<double>(info.frameCount) * 1000.0 / static_cast<double>(info.sampleRate);
    info.peak = peak;
    info.rms = sampleCount == 0 ? 0.0f : static_cast<float>(std::sqrt(sumSquares / static_cast<double>(sampleCount)));
    info.ok = true;
    return info;
}

std::string AssetInventory::ComputeFileSha256(const fs::path& path, bool* ok) {
    if (ok) *ok = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    Sha256 sha;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count > 0) sha.Update(buffer.data(), static_cast<size_t>(count));
    }
    if (!in.eof()) return {};
    if (ok) *ok = true;
    return sha.Finish();
}

bool AssetInventory::Scan(std::string* error) {
    m_records.clear();
    m_summary = {};

    std::error_code ec;
    if (!fs::exists(m_assetRoot, ec) || !fs::is_directory(m_assetRoot, ec)) {
        if (error) *error = "Asset root is not a directory: " + m_assetRoot.string();
        return false;
    }

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(m_assetRoot, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            if (error) *error = "Could not scan asset root: " + ec.message();
            return false;
        }
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
        return a.lexically_relative(m_assetRoot).generic_string() < b.lexically_relative(m_assetRoot).generic_string();
    });

    m_summary.discoveredFiles = files.size();
    for (const fs::path& path : files) {
        const fs::path relative = path.lexically_relative(m_assetRoot);
        if (IsIgnoredPath(relative)) {
            ++m_summary.ignoredFiles;
            continue;
        }

        AssetRecord record;
        record.relativePath = relative.generic_string();
        record.audio = ReadPcmWavInfo(path);
        bool hashOk = false;
        record.sha256 = ComputeFileSha256(path, &hashOk);
        if (hashOk && record.sha256.size() >= 16) record.assetId = "asset_" + record.sha256.substr(0, 16);

        const Classification classification = Classify(relative);
        record.label = classification.label;
        record.subtype = classification.subtype;
        record.weapon = classification.weapon;
        record.surface = classification.surface;
        record.distance = classification.distance;
        record.sourceGroup = classification.sourceGroup;
        record.classificationRule = classification.rule;
        record.classificationConfidence = classification.confidence;
        record.included = record.audio.ok && hashOk;
        if (!hashOk && record.audio.error.empty()) record.audio.error = "could not hash file";
        if (!record.audio.ok) {
            record.classificationRule = "invalid-wav";
            record.classificationConfidence = 0.0f;
            ++m_summary.invalidWavFiles;
        } else {
            ++m_summary.validWavFiles;
            m_summary.totalDurationSeconds += record.audio.durationMs / 1000.0;
            switch (record.label) {
            case AssetLabel::Gunshot: ++m_summary.gunshots; break;
            case AssetLabel::Footstep: ++m_summary.footsteps; break;
            case AssetLabel::Mechanical: ++m_summary.mechanical; break;
            case AssetLabel::Other: ++m_summary.other; break;
            }
        }
        m_records.push_back(std::move(record));
    }

    std::unordered_map<std::string, std::string> canonicalByHash;
    for (AssetRecord& record : m_records) {
        if (!record.sha256.empty()) {
            const auto [it, inserted] = canonicalByHash.emplace(record.sha256, record.relativePath);
            if (!inserted) {
                record.duplicateOf = it->second;
                ++m_summary.duplicateFiles;
            }
        }
        if (NeedsReview(record)) ++m_summary.reviewNeeded;
    }
    return true;
}

bool AssetInventory::Export(const fs::path& outputDirectory, std::string* error) const {
    std::error_code ec;
    fs::create_directories(outputDirectory, ec);
    if (ec) {
        if (error) *error = "Could not create output directory: " + ec.message();
        return false;
    }

    if (!WriteCsv(outputDirectory / "asset_manifest.csv", m_records, false, error)) return false;
    if (!WriteCsv(outputDirectory / "review_needed.csv", m_records, true, error)) return false;

    const fs::path summaryPath = outputDirectory / "summary.json";
    std::ofstream out(summaryPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "Could not open output: " + summaryPath.string();
        return false;
    }
    out << "{\n"
        << "  \"asset_root\": \"" << JsonEscape(fs::absolute(m_assetRoot).generic_string()) << "\",\n"
        << "  \"discovered_files\": " << m_summary.discoveredFiles << ",\n"
        << "  \"ignored_files\": " << m_summary.ignoredFiles << ",\n"
        << "  \"valid_wav_files\": " << m_summary.validWavFiles << ",\n"
        << "  \"invalid_wav_files\": " << m_summary.invalidWavFiles << ",\n"
        << "  \"duplicate_files\": " << m_summary.duplicateFiles << ",\n"
        << "  \"review_needed\": " << m_summary.reviewNeeded << ",\n"
        << "  \"total_duration_seconds\": " << std::fixed << std::setprecision(6) << m_summary.totalDurationSeconds << ",\n"
        << "  \"labels\": {\n"
        << "    \"gunshot\": " << m_summary.gunshots << ",\n"
        << "    \"footstep\": " << m_summary.footsteps << ",\n"
        << "    \"mechanical\": " << m_summary.mechanical << ",\n"
        << "    \"other\": " << m_summary.other << "\n"
        << "  }\n"
        << "}\n";
    if (!out.good()) {
        if (error) *error = "Failed while writing output: " + summaryPath.string();
        return false;
    }
    return true;
}

} // namespace EchoRadar
