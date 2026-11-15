// Command line of drums-engine.
#pragma once

#include <optional>
#include <string>

namespace drums::engine {

struct Options {
    int port = 7878;
    std::string kit;        // kit.json (default: kits/avl-black-pearl or the first kit found)
    std::string exercise;   // exercise JSON (default: exercises/groove-12-8.json)
    std::string padMap;     // default: kits/pad-map.json
    int buffer = 128;
    std::string midiMatch = "e-drum";
    std::string audioMatch;
    std::string clientName = "drums-engine";
    bool autoconnect = true;
    std::string root;       // default: detected from the executable location
    bool listPorts = false;
    std::string offlineRender;  // output .wav
    double seconds = 10.0;

    // Initial transport/mixer settings (the UI can change them later).
    std::optional<double> bpm;
    std::optional<int> countIn;
    std::optional<bool> loop;
    std::optional<bool> click;
    std::optional<bool> subdivisions;
    std::optional<bool> groove;
    bool play = false;      // start the transport right away
    bool logHits = false;   // print every pad hit (debug)
    bool trace = false;     // offline render: print the click/groove schedule
};

// Parses argv. On --help prints usage and exits(0); on bad input prints an error and exits(2).
Options parseOptions(int argc, char** argv);

// Repo root: --root, else walk up from the executable until kits/ and exercises/ exist, else cwd.
std::string detectRoot(const std::string& argv0);

}  // namespace drums::engine
