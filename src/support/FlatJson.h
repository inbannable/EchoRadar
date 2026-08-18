#pragma once

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace EchoRadar::detail {

inline std::string ReadFileToString(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

inline std::map<std::string, std::string> ParseFlatJson(const std::string& text) {
    std::map<std::string, std::string> values;
    size_t cursor = 0;
    const auto skipWhitespace = [&](size_t position) {
        while (position < text.size() &&
               std::isspace(static_cast<unsigned char>(text[position]))) {
            ++position;
        }
        return position;
    };
    while (cursor < text.size()) {
        cursor = skipWhitespace(cursor);
        if (cursor >= text.size() || text[cursor] != '"') {
            ++cursor;
            continue;
        }
        const size_t keyStart = ++cursor;
        while (cursor < text.size() && text[cursor] != '"') {
            if (text[cursor] == '\\' && cursor + 1 < text.size()) ++cursor;
            ++cursor;
        }
        if (cursor >= text.size()) break;
        const std::string key = text.substr(keyStart, cursor - keyStart);
        cursor = skipWhitespace(cursor + 1);
        if (cursor >= text.size() || text[cursor] != ':') continue;
        cursor = skipWhitespace(cursor + 1);
        if (cursor >= text.size()) break;

        std::string value;
        if (text[cursor] == '"') {
            ++cursor;
            while (cursor < text.size() && text[cursor] != '"') {
                if (text[cursor] == '\\' && cursor + 1 < text.size()) {
                    ++cursor;
                    switch (text[cursor]) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    default: value.push_back(text[cursor]); break;
                    }
                } else {
                    value.push_back(text[cursor]);
                }
                ++cursor;
            }
            if (cursor < text.size()) ++cursor;
        } else {
            const size_t valueStart = cursor;
            while (cursor < text.size() && text[cursor] != ',' &&
                   text[cursor] != '\n' && text[cursor] != '\r' &&
                   text[cursor] != '}') {
                ++cursor;
            }
            value = text.substr(valueStart, cursor - valueStart);
            while (!value.empty() &&
                   std::isspace(static_cast<unsigned char>(value.back()))) {
                value.pop_back();
            }
        }
        values[key] = std::move(value);
    }
    return values;
}

inline std::string GetStr(const std::map<std::string, std::string>& values,
                          const std::string& key,
                          const std::string& fallback = {}) {
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
}

inline uint64_t GetU64(const std::map<std::string, std::string>& values,
                       const std::string& key, uint64_t fallback = 0) {
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) return fallback;
    try {
        size_t consumed = 0;
        const uint64_t parsed = std::stoull(found->second, &consumed);
        return consumed == found->second.size() ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

inline float GetFloatVal(const std::map<std::string, std::string>& values,
                         const std::string& key, float fallback = 0.0f) {
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) return fallback;
    try {
        size_t consumed = 0;
        const float parsed = std::stof(found->second, &consumed);
        return consumed == found->second.size() ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

inline bool GetBoolVal(const std::map<std::string, std::string>& values,
                       const std::string& key, bool fallback = false) {
    const auto found = values.find(key);
    if (found == values.end()) return fallback;
    if (found->second == "true" || found->second == "1") return true;
    if (found->second == "false" || found->second == "0") return false;
    return fallback;
}

inline std::string JsonEscapeStr(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

} // namespace EchoRadar::detail
