#include "kit.h"

#include "log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace drums::engine {

KitConfig loadKitConfig(const std::string& jsonPath) {
    std::ifstream in(jsonPath);
    if (!in) throw std::runtime_error("cannot open kit file " + jsonPath);
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(in);
    } catch (const std::exception& e) {
        throw std::runtime_error(jsonPath + ": invalid JSON: " + e.what());
    }
    if (!j.is_object()) throw std::runtime_error(jsonPath + ": expected an object");

    KitConfig kit;
    kit.jsonPath = fs::absolute(jsonPath).lexically_normal().string();
    const fs::path dir = fs::path(kit.jsonPath).parent_path();
    kit.name = j.value("name", dir.filename().string());
    if (!j.contains("sfz") || !j["sfz"].is_string())
        throw std::runtime_error(jsonPath + ": missing \"sfz\"");
    fs::path sfz = j["sfz"].get<std::string>();
    if (sfz.is_relative()) sfz = dir / sfz;
    kit.sfzPath = sfz.lexically_normal().string();
    if (!fs::exists(kit.sfzPath)) throw std::runtime_error(jsonPath + ": sfz not found: " + kit.sfzPath);

    if (!j.contains("notes") || !j["notes"].is_object())
        throw std::runtime_error(jsonPath + ": missing \"notes\" object");
    for (const auto& [name, value] : j["notes"].items()) {
        const Instrument instrument = instrumentFromString(name);
        if (instrument == Instrument::Unknown)
            throw std::runtime_error(jsonPath + ": unknown instrument \"" + name + "\"");
        if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 127)
            throw std::runtime_error(jsonPath + ": note for \"" + name + "\" must be 0-127");
        kit.notes[instrumentIndex(instrument)] = value.get<int>();
    }
    if (j.contains("hihatCC") && j["hihatCC"].is_number_integer())
        kit.hihatCC = std::clamp(j["hihatCC"].get<int>(), 0, 127);
    kit.license = j.value("license", "");
    kit.source = j.value("source", "");
    return kit;
}

std::vector<KitListEntry> scanKits(const std::string& root) {
    std::vector<KitListEntry> kits;
    const fs::path kitsDir = fs::path(root) / "kits";
    std::error_code ec;
    if (!fs::is_directory(kitsDir, ec)) return kits;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(kitsDir, ec)) {
        const fs::path json = entry.path() / "kit.json";
        if (entry.is_directory(ec) && fs::is_regular_file(json, ec)) files.push_back(json);
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        try {
            std::ifstream in(file);
            const auto j = nlohmann::json::parse(in);
            kits.push_back({j.value("name", file.parent_path().filename().string()),
                            displayPath(root, file.string())});
        } catch (const std::exception& e) {
            logWarn("skipping kit %s: %s", file.c_str(), e.what());
        }
    }
    return kits;
}

std::string resolvePath(const std::string& root, const std::string& path) {
    fs::path p(path);
    if (p.is_relative()) p = fs::path(root) / p;
    return p.lexically_normal().string();
}

std::string displayPath(const std::string& root, const std::string& path) {
    const fs::path abs = fs::absolute(path).lexically_normal();
    const fs::path rel = abs.lexically_relative(fs::absolute(root).lexically_normal());
    if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
    return abs.string();
}

std::string defaultKitPath(const std::string& root) {
    const fs::path preferred = fs::path(root) / "kits" / "avl-black-pearl" / "kit.json";
    std::error_code ec;
    if (fs::is_regular_file(preferred, ec)) return preferred.string();
    const auto kits = scanKits(root);
    return kits.empty() ? std::string() : resolvePath(root, kits.front().path);
}

}  // namespace drums::engine
