// Test tool: a JACK client that records the engine's audio output AND the same MIDI source the
// engine listens to, on the same JACK frame clock. Because it runs in the same cycle right after the
// engine, a note whose MIDI event arrives at frame F must show up in the captured audio at frame F
// (plus the sample's own pre-roll) if the engine triggers sfizz sample-accurately.
//
//   jack-probe --seconds S --wav out.wav --midi-log midi.txt --audio drums-engine:out_L,drums-engine:out_R
//              --midi-match e-drum-sim [--clock-from "Built-in Audio Analog Stereo:monitor_FL"]
// --clock-from links an unused input from a device port (e.g. a sink's monitor), so the graph is
// driven by that device's clock instead of PipeWire's timer-based Dummy-Driver. Nothing is played.
//
// midi.txt lines: "<frame since capture start> <status hex> <data1> <data2>" (clock 0xF8 omitted).
#include <jack/jack.h>
#include <jack/midiport.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct MidiRecord {
    int64_t frame;
    uint8_t bytes[3];
};

jack_client_t* gClient = nullptr;
jack_port_t* gMidi = nullptr;
jack_port_t* gIn[2] = {nullptr, nullptr};
jack_port_t* gClockIn = nullptr;  // --clock-from: unused input that ties us to a device's driver
std::vector<float> gAudio[2];
std::vector<MidiRecord> gMidiLog;
std::atomic<int64_t> gWritten{0};
std::atomic<int> gMidiCount{0};
int64_t gStartFrame = -1;
jack_nframes_t gExpected = 0;
std::atomic<int> gJumps{0};
std::atomic<int64_t> gJumpFrames{0};
std::atomic<int64_t> gCycles{0};

int process(jack_nframes_t n, void*) {
    const jack_nframes_t now = jack_last_frame_time(gClient);
    if (gStartFrame >= 0 && now != gExpected) {
        const int32_t jump = int32_t(now - gExpected);
        if (jump < 0 || jump > 48000) {
            gStartFrame += jump;  // another driver took over (other frame counter): continue seamlessly
        } else {
            gJumps.fetch_add(1);  // the driver skipped cycles: leave the gap, stay on its clock
            gJumpFrames.fetch_add(jump);
        }
    }
    gExpected = now + n;
    gCycles.fetch_add(1);
    const int64_t cycleStart = now;
    if (gStartFrame < 0) gStartFrame = cycleStart;
    if (cycleStart - gStartFrame < 0) return 0;
    const int64_t at = cycleStart - gStartFrame;
    const int64_t room = int64_t(gAudio[0].size()) - at;
    const int64_t count = std::clamp<int64_t>(room, 0, n);
    for (int c = 0; c < 2; ++c) {
        auto* src = static_cast<const float*>(jack_port_get_buffer(gIn[c], n));
        if (count > 0) std::memcpy(gAudio[c].data() + at, src, size_t(count) * sizeof(float));
    }
    void* mb = jack_port_get_buffer(gMidi, n);
    const uint32_t events = jack_midi_get_event_count(mb);
    for (uint32_t i = 0; i < events; ++i) {
        jack_midi_event_t ev;
        if (jack_midi_event_get(&ev, mb, i) != 0 || ev.size == 0 || ev.buffer[0] >= 0xF0) continue;
        const int idx = gMidiCount.load();
        if (idx >= int(gMidiLog.size())) break;
        MidiRecord& r = gMidiLog[size_t(idx)];
        r.frame = at + ev.time;
        std::memset(r.bytes, 0, 3);
        std::memcpy(r.bytes, ev.buffer, std::min<size_t>(ev.size, 3));
        gMidiCount.store(idx + 1);
    }
    gWritten.store(at + count);
    return 0;
}

bool writeWav(const std::string& path, int rate, int64_t frames) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t data = uint32_t(frames) * 8;
    std::fwrite("RIFF", 1, 4, f);
    u32(4 + 26 + 12 + 8 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(18); u16(3); u16(2); u32(uint32_t(rate)); u32(uint32_t(rate) * 8); u16(8); u16(32); u16(0);
    std::fwrite("fact", 1, 4, f);
    u32(4); u32(uint32_t(frames));
    std::fwrite("data", 1, 4, f);
    u32(data);
    for (int64_t i = 0; i < frames; ++i) {
        std::fwrite(&gAudio[0][size_t(i)], 4, 1, f);
        std::fwrite(&gAudio[1][size_t(i)], 4, 1, f);
    }
    std::fclose(f);
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 5;
    std::string wav = "probe.wav", midiLog = "probe-midi.txt", audio = "drums-engine:out_L,drums-engine:out_R",
                midiMatch = "e-drum-sim", clockFrom;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--wav") wav = next();
        else if (a == "--midi-log") midiLog = next();
        else if (a == "--audio") audio = next();
        else if (a == "--midi-match") midiMatch = next();
        else if (a == "--clock-from") clockFrom = next();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    jack_status_t status{};
    gClient = jack_client_open("drums-probe", JackNoStartServer, &status);
    if (!gClient) {
        std::fprintf(stderr, "jack-probe: no JACK server\n");
        return 1;
    }
    const int rate = int(jack_get_sample_rate(gClient));
    gAudio[0].assign(size_t(seconds * rate), 0.0f);
    gAudio[1].assign(size_t(seconds * rate), 0.0f);
    gMidiLog.resize(4096);
    gMidi = jack_port_register(gClient, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    gIn[0] = jack_port_register(gClient, "in_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    gIn[1] = jack_port_register(gClient, "in_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (!clockFrom.empty())
        gClockIn = jack_port_register(gClient, "clock_in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    jack_set_process_callback(gClient, process, nullptr);
    if (jack_activate(gClient) != 0) return 1;

    if (gClockIn && jack_connect(gClient, clockFrom.c_str(), jack_port_name(gClockIn)) == 0)
        std::fprintf(stderr, "jack-probe: clock_in <- %s (its driver now drives this graph)\n", clockFrom.c_str());
    const auto comma = audio.find(',');
    const std::string srcL = audio.substr(0, comma), srcR = comma == std::string::npos ? srcL : audio.substr(comma + 1);
    int rc = jack_connect(gClient, srcL.c_str(), jack_port_name(gIn[0]));
    rc |= jack_connect(gClient, srcR.c_str(), jack_port_name(gIn[1]));
    if (rc != 0) std::fprintf(stderr, "jack-probe: could not connect %s / %s\n", srcL.c_str(), srcR.c_str());
    const char** ports = jack_get_ports(gClient, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
    for (const char** p = ports; p && *p; ++p) {
        if (lower(*p).find(lower(midiMatch)) == std::string::npos) continue;
        if (jack_connect(gClient, *p, jack_port_name(gMidi)) == 0) std::fprintf(stderr, "jack-probe: MIDI <- %s\n", *p);
    }
    if (ports) jack_free(ports);
    std::fprintf(stderr, "jack-probe: %d Hz, buffer %u, recording %.1f s\n", rate, jack_get_buffer_size(gClient), seconds);

    while (gWritten.load() < int64_t(gAudio[0].size())) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    jack_deactivate(gClient);
    jack_client_close(gClient);

    writeWav(wav, rate, int64_t(gAudio[0].size()));
    FILE* f = std::fopen(midiLog.c_str(), "w");
    for (int i = 0; i < gMidiCount.load(); ++i) {
        const auto& r = gMidiLog[size_t(i)];
        std::fprintf(f, "%lld %02x %d %d\n", static_cast<long long>(r.frame), r.bytes[0], r.bytes[1], r.bytes[2]);
    }
    std::fclose(f);
    std::fprintf(stderr, "jack-probe: wrote %s and %s (%d MIDI events)\n", wav.c_str(), midiLog.c_str(),
                 gMidiCount.load());
    std::fprintf(stderr, "jack-probe: %lld cycles, frame-time jumps: %d (%lld frames)\n",
                 static_cast<long long>(gCycles.load()), gJumps.load(), static_cast<long long>(gJumpFrames.load()));
    return 0;
}
