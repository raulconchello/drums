#include "options.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace fs = std::filesystem;

namespace drums::engine {
namespace {

constexpr const char* kUsage = R"(drums-engine — real-time drum practice audio engine (JACK/PipeWire + sfizz)

Usage: drums-engine [options]

  --port N               WebSocket control port (default 7878)
  --kit PATH             kit.json to load (default kits/avl-black-pearl/kit.json, else first kit)
  --exercise PATH        exercise JSON (default exercises/groove-12-8.json)
  --pad-map PATH         e-drum note map (default kits/pad-map.json)
  --buffer N             requested period in frames, sets PIPEWIRE_QUANTUM=N/48000 (default 128)
  --midi-match TEXT      auto-connect MIDI sources whose name contains TEXT (default "e-drum")
  --audio-match TEXT     prefer playback ports whose name contains TEXT (default: first device)
  --client-name NAME     JACK client name (default drums-engine)
  --no-autoconnect       do not connect any ports
  --root DIR             repo root (default: detected from the executable location)
  --list-ports           list MIDI sources and playback ports, then exit
  --offline-render WAV   render click + groove without JACK into a 32-bit float WAV, then exit
  --seconds N            length of --offline-render (default 10)

Initial settings (the UI can change them):
  --bpm N  --count-in BARS  --loop | --no-loop  --click | --no-click
  --subdivisions | --no-subdivisions  --groove | --no-groove
  --play                 start the transport immediately
  --log-hits             print every pad hit on stderr
  --trace                with --offline-render: print every scheduled click/groove note
  -h, --help
)";

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "drums-engine: %s (see --help)\n", message.c_str());
    std::exit(2);
}

double toNumber(std::string_view flag, const char* text) {
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (!end || *end != '\0' || end == text) fail(std::string(flag) + " expects a number, got '" + text + "'");
    return v;
}

}  // namespace

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) fail(std::string(a) + " needs a value");
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            std::exit(0);
        } else if (a == "--port") {
            const double p = toNumber(a, value());
            if (p < 1 || p > 65535) fail("--port out of range");
            o.port = int(p);
        } else if (a == "--kit") {
            o.kit = value();
        } else if (a == "--exercise") {
            o.exercise = value();
        } else if (a == "--pad-map") {
            o.padMap = value();
        } else if (a == "--buffer") {
            const double b = toNumber(a, value());
            if (b < 16 || b > 8192) fail("--buffer must be 16-8192");
            o.buffer = int(b);
        } else if (a == "--midi-match") {
            o.midiMatch = value();
        } else if (a == "--audio-match") {
            o.audioMatch = value();
        } else if (a == "--client-name") {
            o.clientName = value();
        } else if (a == "--no-autoconnect") {
            o.autoconnect = false;
        } else if (a == "--root") {
            o.root = value();
        } else if (a == "--list-ports") {
            o.listPorts = true;
        } else if (a == "--offline-render") {
            o.offlineRender = value();
        } else if (a == "--seconds") {
            o.seconds = toNumber(a, value());
            if (o.seconds <= 0 || o.seconds > 3600) fail("--seconds must be in (0, 3600]");
        } else if (a == "--bpm") {
            o.bpm = toNumber(a, value());
            if (*o.bpm < 20 || *o.bpm > 300) fail("--bpm must be 20-300");
        } else if (a == "--count-in") {
            const double c = toNumber(a, value());
            if (c < 0 || c > 2) fail("--count-in must be 0-2");
            o.countIn = int(c);
        } else if (a == "--loop" || a == "--no-loop") {
            o.loop = a == "--loop";
        } else if (a == "--click" || a == "--no-click") {
            o.click = a == "--click";
        } else if (a == "--subdivisions" || a == "--no-subdivisions") {
            o.subdivisions = a == "--subdivisions";
        } else if (a == "--groove" || a == "--no-groove") {
            o.groove = a == "--groove";
        } else if (a == "--play") {
            o.play = true;
        } else if (a == "--log-hits") {
            o.logHits = true;
        } else if (a == "--trace") {
            o.trace = true;
        } else {
            fail("unknown option '" + std::string(a) + "'");
        }
    }
    return o;
}

std::string detectRoot(const std::string& argv0) {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) exe = fs::absolute(argv0, ec);
    for (fs::path dir = exe.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_directory(dir / "kits", ec) && fs::is_directory(dir / "exercises", ec)) return dir.string();
        if (dir == dir.root_path()) break;
    }
    return fs::current_path(ec).string();
}

}  // namespace drums::engine
