#include "jack_backend.h"

#include "log.h"

#include <jack/midiport.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace drums::engine {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Port name plus its aliases (PipeWire puts the ALSA device name in them).
std::vector<std::string> namesOf(jack_client_t* client, const char* portName) {
    std::vector<std::string> names{portName};
    if (jack_port_t* port = jack_port_by_name(client, portName)) {
        const int size = jack_port_name_size();
        std::vector<char> a(static_cast<size_t>(size)), b(static_cast<size_t>(size));
        char* aliases[2] = {a.data(), b.data()};
        const int n = jack_port_get_aliases(port, aliases);
        for (int i = 0; i < n; ++i) names.emplace_back(aliases[i]);
    }
    return names;
}

bool matches(jack_client_t* client, const char* portName, const std::string& needle) {
    if (needle.empty()) return false;
    const std::string n = lower(needle);
    for (const auto& name : namesOf(client, portName))
        if (lower(name).find(n) != std::string::npos) return true;
    return false;
}

struct PortList {
    const char** ports = nullptr;
    explicit PortList(const char** p) : ports(p) {}
    ~PortList() {
        if (ports) jack_free(ports);
    }
    PortList(const PortList&) = delete;
    PortList& operator=(const PortList&) = delete;
};

jack_latency_range_t latencyOf(jack_port_t* port, jack_latency_callback_mode_t mode) {
    jack_latency_range_t range{0, 0};
    jack_port_get_latency_range(port, mode, &range);
    return range;
}

}  // namespace

JackBackend::~JackBackend() { stop(); }

bool JackBackend::open(const Config& config, std::string* error) {
    config_ = config;
    // PipeWire: ask the graph for our period. Must be set before jack_client_open; the rate part is
    // the graph rate (48 kHz here, clock.allowed-rates). An explicit user setting wins.
    char quantum[64];
    std::snprintf(quantum, sizeof quantum, "%d/48000", config.bufferFrames);
    setenv("PIPEWIRE_QUANTUM", quantum, 0);
    logInfo("PIPEWIRE_QUANTUM=%s", std::getenv("PIPEWIRE_QUANTUM"));

    jack_status_t status{};
    client_ = jack_client_open(config.clientName.c_str(), JackNoStartServer, &status);
    if (!client_) {
        if (error) *error = "cannot connect to the JACK/PipeWire server (status 0x" + std::to_string(int(status)) + ")";
        return false;
    }
    sampleRate_ = int(jack_get_sample_rate(client_));
    bufferSize_ = int(jack_get_buffer_size(client_));
    midiIn_ = jack_port_register(client_, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    outL_ = jack_port_register(client_, "out_L", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    outR_ = jack_port_register(client_, "out_R", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!midiIn_ || !outL_ || !outR_) {
        if (error) *error = "cannot register JACK ports";
        return false;
    }
    return true;
}

bool JackBackend::start(AudioEngine* engine, std::string* error) {
    engine_ = engine;
    jack_set_process_callback(client_, &JackBackend::processCallback, this);
    jack_set_buffer_size_callback(client_, &JackBackend::bufferSizeCallback, this);
    jack_set_xrun_callback(client_, &JackBackend::xrunCallback, this);
    jack_set_graph_order_callback(client_, &JackBackend::graphOrderCallback, this);
    jack_set_port_registration_callback(client_, &JackBackend::portRegistrationCallback, this);
    jack_on_shutdown(client_, &JackBackend::shutdownCallback, this);
    if (jack_activate(client_) != 0) {
        if (error) *error = "cannot activate the JACK client";
        return false;
    }
    active_ = true;
    if (config_.autoconnect) {
        connectMidi();
        connectAudio();
    } else {
        logInfo("auto-connect disabled: connect %s:midi_in and %s:out_L/out_R yourself",
                jack_get_client_name(client_), jack_get_client_name(client_));
    }
    bufferSize_ = int(jack_get_buffer_size(client_));
    loggedBufferSize_ = bufferSize_;
    updateLatency(false);
    logInfo("JACK client '%s': sample rate %d Hz, buffer %d frames (%.2f ms), output latency %d frames (%.2f ms)",
            jack_get_client_name(client_), sampleRate_, bufferSize_.load(), bufferSize_ * 1000.0 / sampleRate_,
            outputLatency_.load(), outputLatencyMs());
    return true;
}

void JackBackend::updateLatency(bool log) {
    const jack_latency_range_t out = latencyOf(outL_, JackPlaybackLatency);
    const jack_latency_range_t in = latencyOf(midiIn_, JackCaptureLatency);
    const int previous = outputLatency_.exchange(int(out.max));
    if (engine_) engine_->setOutputLatencyFrames(int(out.max));
    if (!log || previous != int(out.max) || midiCaptureLatency_ != int(in.max)) {
        logInfo("latency: output port playback %u..%u frames (%.2f ms) [used for hit compensation], "
                "MIDI input capture %u..%u frames (%.2f ms)",
                out.min, out.max, out.max * 1000.0 / sampleRate_, in.min, in.max, in.max * 1000.0 / sampleRate_);
    }
    midiCaptureLatency_ = int(in.max);
}

void JackBackend::connectMidi() {
    PortList ports(jack_get_ports(client_, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput));
    if (!ports.ports) return;
    const std::string self = jack_port_name(midiIn_);
    for (const char** p = ports.ports; *p; ++p) {
        jack_port_t* port = jack_port_by_name(client_, *p);
        if (!port || jack_port_is_mine(client_, port)) continue;
        if (!matches(client_, *p, config_.midiMatch) || jack_port_connected_to(midiIn_, *p)) continue;
        const int rc = jack_connect(client_, *p, self.c_str());
        if (rc == 0 || rc == EEXIST) logInfo("MIDI in <- %s", *p);
        else logWarn("could not connect MIDI %s -> %s (%d)", *p, self.c_str(), rc);
    }
}

void JackBackend::connectAudio() {
    PortList ports(jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsPhysical));
    if (!ports.ports || !ports.ports[0]) {
        logWarn("no physical audio playback ports found");
        return;
    }
    std::vector<std::string> candidates;
    for (const char** p = ports.ports; *p; ++p)
        if (config_.audioMatch.empty() || matches(client_, *p, config_.audioMatch)) candidates.emplace_back(*p);
    if (candidates.empty()) {
        logWarn("no playback port matches --audio-match '%s': audio outputs left unconnected",
                config_.audioMatch.c_str());
        audioGivenUp_ = true;
        return;
    }
    // Two channels of the same device as the first candidate.
    const std::string device = candidates[0].substr(0, candidates[0].find(':'));
    std::vector<std::string> sameDevice;
    for (const auto& c : candidates)
        if (c.compare(0, device.size() + 1, device + ":") == 0) sameDevice.push_back(c);
    const std::string left = sameDevice[0];
    const std::string right = sameDevice.size() > 1 ? sameDevice[1] : sameDevice[0];
    const int a = jack_connect(client_, jack_port_name(outL_), left.c_str());
    const int b = jack_connect(client_, jack_port_name(outR_), right.c_str());
    audioConnected_ = (a == 0 || a == EEXIST) && (b == 0 || b == EEXIST);
    if (audioConnected_) logInfo("audio out -> %s, %s", left.c_str(), right.c_str());
    else logWarn("could not connect audio outputs (%d, %d)", a, b);
}

bool JackBackend::poll() {
    if (!client_ || !active_) return false;
    if (shutdown_.load()) {
        logError("the JACK/PipeWire server shut the client down");
        return false;
    }
    if (portsChanged_.exchange(false) && config_.autoconnect) {
        connectMidi();
        if (!audioConnected_ && !audioGivenUp_) connectAudio();
    }
    if (graphChanged_.exchange(false)) updateLatency(true);
    if (const int bs = bufferSize_.load(); bs != loggedBufferSize_) {
        logInfo("buffer size changed: %d frames (%.2f ms)", bs, bs * 1000.0 / sampleRate_);
        loggedBufferSize_ = bs;
        updateLatency(true);
    }
    if (const uint64_t x = xruns_.load(); x != loggedXruns_) {
        logWarn("xruns: %llu", static_cast<unsigned long long>(x));
        loggedXruns_ = x;
    }
    if (const uint64_t d = discontinuities_.load(); d != loggedDiscontinuities_) {
        logWarn("JACK frame time jumped %llu time(s): %lld frames passed without a process() call",
                static_cast<unsigned long long>(d), static_cast<long long>(skippedFrames_.load()));
        loggedDiscontinuities_ = d;
    }
    return true;
}

void JackBackend::stop() {
    if (!client_) return;
    if (active_) {
        const double periodNs = busyFrames_.load() * 1e9 / sampleRate_;
        logInfo("process cycles: %llu, frame-time jumps: %llu (%lld frames), xruns: %llu, "
                "callback load avg %.2f%% max %.1f%% of the period",
                static_cast<unsigned long long>(cycles_.load()), static_cast<unsigned long long>(discontinuities_.load()),
                static_cast<long long>(skippedFrames_.load()), static_cast<unsigned long long>(xruns_.load()),
                periodNs > 0 ? 100.0 * double(busyNs_.load()) / periodNs : 0.0, maxLoadPermille_.load() / 10.0);
        jack_deactivate(client_);
    }
    active_ = false;
    jack_client_close(client_);
    client_ = nullptr;
}

// ---------------------------------------------------------------- callbacks

int JackBackend::processCallback(jack_nframes_t frames, void* arg) {
    auto* self = static_cast<JackBackend*>(arg);
    // Cycle continuity: frames the graph advanced without calling us (diagnostics only).
    const jack_nframes_t cycleStart = jack_last_frame_time(self->client_);
    if (self->cycles_.load(std::memory_order_relaxed) > 0 && cycleStart != self->expectedCycleStart_) {
        const int32_t jump = int32_t(cycleStart - self->expectedCycleStart_);
        if (jump > 0 && jump < self->sampleRate_) {  // else: another driver took over (new frame counter)
            self->discontinuities_.fetch_add(1, std::memory_order_relaxed);
            self->skippedFrames_.fetch_add(jump, std::memory_order_relaxed);
        }
    }
    self->expectedCycleStart_ = cycleStart + frames;
    self->cycles_.fetch_add(1, std::memory_order_relaxed);
    void* midiBuffer = jack_port_get_buffer(self->midiIn_, frames);
    const uint32_t count = jack_midi_get_event_count(midiBuffer);
    int n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        jack_midi_event_t ev;
        if (jack_midi_event_get(&ev, midiBuffer, i) != 0 || ev.size == 0) continue;
        if (ev.buffer[0] >= 0xF0) continue;  // MIDI clock (0xF8) stream, active sensing, sysex: ignored
        if (n == kMaxMidiEvents) {
            self->midiOverflow_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        MidiEvent& m = self->midi_[size_t(n++)];
        m.frame = std::min<uint32_t>(ev.time, frames - 1);
        m.size = uint8_t(std::min<size_t>(ev.size, 3));
        std::memcpy(m.data, ev.buffer, m.size);
    }
    auto* left = static_cast<float*>(jack_port_get_buffer(self->outL_, frames));
    auto* right = static_cast<float*>(jack_port_get_buffer(self->outR_, frames));
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);  // vDSO, no syscall
    self->engine_->process(int(frames), self->midi_.data(), n, left, right);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const int64_t ns = (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    self->busyNs_.fetch_add(ns, std::memory_order_relaxed);
    self->busyFrames_.fetch_add(frames, std::memory_order_relaxed);
    // max as a fraction of the period, in 1/1000
    const int64_t permille = ns * self->sampleRate_ / (int64_t(frames) * 1000000LL);
    if (permille > self->maxLoadPermille_.load(std::memory_order_relaxed))
        self->maxLoadPermille_.store(permille, std::memory_order_relaxed);
    return 0;
}

int JackBackend::bufferSizeCallback(jack_nframes_t frames, void* arg) {
    static_cast<JackBackend*>(arg)->bufferSize_.store(int(frames));
    return 0;
}

int JackBackend::xrunCallback(void* arg) {
    static_cast<JackBackend*>(arg)->xruns_.fetch_add(1);
    return 0;
}

int JackBackend::graphOrderCallback(void* arg) {
    static_cast<JackBackend*>(arg)->graphChanged_.store(true);
    return 0;
}

void JackBackend::portRegistrationCallback(jack_port_id_t, int registered, void* arg) {
    if (registered) static_cast<JackBackend*>(arg)->portsChanged_.store(true);
}

void JackBackend::shutdownCallback(void* arg) { static_cast<JackBackend*>(arg)->shutdown_.store(true); }

// ---------------------------------------------------------------- --list-ports

int JackBackend::listPorts(const std::string& midiMatch, const std::string& audioMatch) {
    jack_status_t status{};
    jack_client_t* client = jack_client_open("drums-engine-list", JackNoStartServer, &status);
    if (!client) {
        std::fprintf(stderr, "cannot connect to the JACK/PipeWire server\n");
        return 1;
    }
    std::printf("JACK server: %u Hz, buffer %u frames\n", jack_get_sample_rate(client), jack_get_buffer_size(client));
    int found = 0;
    std::printf("\nMIDI sources (outputs) — '*' = matches --midi-match \"%s\":\n", midiMatch.c_str());
    {
        PortList ports(jack_get_ports(client, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput));
        for (const char** p = ports.ports; p && *p; ++p) {
            const bool m = matches(client, *p, midiMatch);
            found += m;
            const auto names = namesOf(client, *p);
            std::printf("  %s %s\n", m ? "*" : " ", *p);
            for (size_t i = 1; i < names.size(); ++i) std::printf("        alias: %s\n", names[i].c_str());
        }
    }
    std::printf("\nAudio playback ports (physical inputs)%s:\n", audioMatch.empty() ? "" : " — '*' = --audio-match");
    {
        PortList ports(jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsPhysical));
        for (const char** p = ports.ports; p && *p; ++p)
            std::printf("  %s %s\n", matches(client, *p, audioMatch) ? "*" : " ", *p);
    }
    std::printf("\n%d MIDI source(s) match \"%s\"\n", found, midiMatch.c_str());
    jack_client_close(client);
    return 0;
}

}  // namespace drums::engine
