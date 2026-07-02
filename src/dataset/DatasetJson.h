#pragma once

// Minimal, dependency-free helpers for reading/writing the flat JSON files
// produced by the dataset recorder (metadata.json). This is intentionally
// NOT a general-purpose JSON library: it only understands a single-level
// object with string/number values, which is all EchoRadar's metadata
// format ever needs.

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace EchoRadar::detail {

inline std::string ReadFileToString(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Parses `"key": value` pairs out of a flat JSON object. Values keep their
// raw textual form (quotes stripped for strings, verbatim for numbers).
inline std::map<std::string, std::string> ParseFlatJson(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    const size_t n = text.size();

    auto skipWs = [&](size_t pos) {
        while (pos < n && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        return pos;
    };

    while (i < n) {
        i = skipWs(i);
        if (i >= n || text[i] != '\"') {
            ++i;
            continue;
        }
        // Parse key
        size_t keyStart = ++i;
        while (i < n && text[i] != '\"') {
            if (text[i] == '\\') ++i;
            ++i;
        }
        if (i >= n) break;
        std::string key = text.substr(keyStart, i - keyStart);
        ++i; // skip closing quote

        i = skipWs(i);
        if (i >= n || text[i] != ':') {
            continue;
        }
        ++i; // skip ':'
        i = skipWs(i);
        if (i >= n) break;

        std::string value;
        if (text[i] == '\"') {
            size_t valStart = ++i;
            std::string raw;
            while (i < n && text[i] != '\"') {
                if (text[i] == '\\' && i + 1 < n) {
                    ++i;
                    switch (text[i]) {
                    case 'n': raw.push_back('\n'); break;
                    case 't': raw.push_back('\t'); break;
                    case 'r': raw.push_back('\r'); break;
                    case '\"': raw.push_back('\"'); break;
                    case '\\': raw.push_back('\\'); break;
                    default: raw.push_back(text[i]); break;
                    }
                } else {
                    raw.push_back(text[i]);
                }
                ++i;
            }
            (void)valStart;
            value = raw;
            if (i < n) ++i; // skip closing quote
        } else {
            size_t valStart = i;
            while (i < n && text[i] != ',' && text[i] != '\n' && text[i] != '}' && text[i] != '\r') {
                ++i;
            }
            value = text.substr(valStart, i - valStart);
            // trim trailing whitespace
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
                value.pop_back();
            }
        }
        out[key] = value;
    }
    return out;
}

inline std::string GetStr(const std::map<std::string, std::string>& m, const std::string& key, const std::string& def = "") {
    auto it = m.find(key);
    return it == m.end() ? def : it->second;
}

inline uint64_t GetU64(const std::map<std::string, std::string>& m, const std::string& key, uint64_t def = 0) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return def;
    try { return std::stoull(it->second); } catch (...) { return def; }
}

inline float GetFloatVal(const std::map<std::string, std::string>& m, const std::string& key, float def = 0.0f) {
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return def;
    try { return std::stof(it->second); } catch (...) { return def; }
}

inline std::string JsonEscapeStr(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace EchoRadar::detail
