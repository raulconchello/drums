// JACK client (PipeWire's JACK API): one MIDI input, stereo audio output, the real-time process
// callback that drives AudioEngine, auto-connection (also when the e-drum is plugged in later) and
// latency reporting. Everything except process() runs on the main thread.
#pragma once

#include "audio_engine.h"

#include <jack/jack.h>

#include <array>
#include <atomic>
#include <string>

namespace drums::engine {

class JackBackend {
public:
    struct Config {
        std::string clientName = "drums-engine";
        int bufferFrames = 128;          // requested period (PIPEWIRE_QUANTUM)
        std::string midiMatch = "e-drum";// auto-connect MIDI sources whose name/alias contains this
        std::string audioMatch;          // optional: prefer playback ports containing this
        bool autoconnect = true;
    };

    JackBackend() = default;
    ~JackBackend();
    JackBackend(const JackBackend&) = delete;
    JackBackend& operator=(const JackBackend&) = delete;

    // Sets PIPEWIRE_QUANTUM (unless already set), opens the client and registers the ports.
    bool open(const Config& config, std::string* error);
    int sampleRate() const { return sampleRate_; }
    int bufferSize() const { return bufferSize_.load(); }
    int outputLatencyFrames() const { return outputLatency_.load(); }
    double outputLatencyMs() const { return outputLatencyFrames() * 1000.0 / sampleRate_; }

    // Activates processing with this engine, auto-connects, logs buffer size and latencies.
    bool start(AudioEngine* engine, std::string* error);
    // Main thread, call every ~50-100 ms: reconnects hot-plugged MIDI devices, tracks latency,
    // buffer size and xruns. Returns false if the JACK server went away.
    bool poll();
    void stop();

    // --list-ports: prints MIDI sources and audio playback ports. Returns a process exit code.
    static int listPorts(const std::string& midiMatch, const std::string& audioMatch);

private:
    static int processCallback(jack_nframes_t frames, void* arg);
    static int bufferSizeCallback(jack_nframes_t frames, void* arg);
    static int xrunCallback(void* arg);
    static int graphOrderCallback(void* arg);
    static void portRegistrationCallback(jack_port_id_t port, int registered, void* arg);
    static void shutdownCallback(void* arg);

    void connectMidi();
    void connectAudio();
    void updateLatency(bool log);

    Config config_;
    jack_client_t* client_ = nullptr;
    jack_port_t* midiIn_ = nullptr;
    jack_port_t* outL_ = nullptr;
    jack_port_t* outR_ = nullptr;
    AudioEngine* engine_ = nullptr;
    int sampleRate_ = 48000;
    bool active_ = false;
    bool audioConnected_ = false;
    bool audioGivenUp_ = false;

    static constexpr int kMaxMidiEvents = 512;
    std::array<MidiEvent, kMaxMidiEvents> midi_{};

    std::atomic<int> bufferSize_{0};
    std::atomic<int> outputLatency_{0};
    std::atomic<bool> portsChanged_{false};
    std::atomic<bool> graphChanged_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<uint64_t> xruns_{0};
    std::atomic<uint64_t> midiOverflow_{0};
    std::atomic<uint64_t> cycles_{0};
    std::atomic<uint64_t> discontinuities_{0};
    std::atomic<int64_t> skippedFrames_{0};
    std::atomic<int64_t> busyNs_{0};
    std::atomic<int64_t> busyFrames_{0};
    std::atomic<int64_t> maxLoadPermille_{0};
    jack_nframes_t expectedCycleStart_ = 0;  // callback thread only
    uint64_t loggedXruns_ = 0;
    uint64_t loggedDiscontinuities_ = 0;
    int loggedBufferSize_ = 0;
    int midiCaptureLatency_ = -1;
};

}  // namespace drums::engine
