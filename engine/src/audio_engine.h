// The real-time part of the engine: transport, metronome, groove playback, pad MIDI -> sampler,
// mixer and hit time-stamping. Driven by the JACK process callback (or the offline renderer):
// process() never allocates, locks, logs or does IO. The app thread talks to it only through two
// lock-free SPSC queues (commands in, events out) and a seqlock-published transport snapshot.
#pragma once

#include "click.h"
#include "drum_sampler.h"
#include "exercise_rt.h"
#include "spsc_queue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace drums::engine {

inline constexpr int kChunkFrames = 1024;  // internal render block; longer periods are split

struct MidiEvent {
    uint32_t frame = 0;  // offset in the process() block
    uint8_t size = 0;
    uint8_t data[3] = {0, 0, 0};
};

// The kit loaded twice: one instance for the pads, one for the groove (so the groove never chokes
// or steals the player's voices, and has its own volume).
struct KitBundle {
    std::unique_ptr<DrumSampler> player;
    std::unique_ptr<DrumSampler> groove;
};

enum class CommandType : uint8_t {
    Play, Stop, SetBpm, SetLoop, SetCountIn, SetClick, SetGroove, SetMasterVolume, SetInstrumentVolume,
    SetLatencyOffset, SetPadNote, SetHihatPedalCC, SetVelocityCurve, SetExercise, SetKit,
};

struct Command {
    CommandType type = CommandType::Stop;
    int32_t index = 0;   // instrument index / note / bars / controller
    int32_t value = 0;   // instrument (SetPadNote)
    double x = 0, y = 0; // bpm / volume / ms / gain, exponent
    bool on = false;
    bool flag = false;   // subdivisions (SetClick)
    uint8_t fields = 0;  // SetClick/SetGroove: bit0 on, bit1 volume, bit2 subdivisions
    void* ptr = nullptr; // RtExercise* / KitBundle*; ownership passes to the engine

    static Command play() { return {CommandType::Play}; }
    static Command stop() { return {CommandType::Stop}; }
    static Command bpm(double bpm) { Command c{CommandType::SetBpm}; c.x = bpm; return c; }
    static Command loop(bool on) { Command c{CommandType::SetLoop}; c.on = on; return c; }
    static Command countIn(int bars) { Command c{CommandType::SetCountIn}; c.index = bars; return c; }
    static Command click(std::optional<bool> on, std::optional<double> volume, std::optional<bool> subdivisions);
    static Command groove(std::optional<bool> on, std::optional<double> volume);
    static Command masterVolume(double v) { Command c{CommandType::SetMasterVolume}; c.x = v; return c; }
    static Command instrumentVolume(Instrument i, double v);
    static Command latencyOffset(double ms) { Command c{CommandType::SetLatencyOffset}; c.x = ms; return c; }
    static Command padNote(int note, Instrument i);
    static Command hihatPedalCC(int cc) { Command c{CommandType::SetHihatPedalCC}; c.index = cc; return c; }
    static Command velocityCurve(Instrument i, double gain, double exponent);
    static Command exercise(std::unique_ptr<RtExercise> ex);
    static Command kit(std::unique_ptr<KitBundle> kit);
};

enum class EventType : uint8_t { Hit, NoteOff, Control, Stopped, Garbage };
enum class GarbageKind : uint8_t { None, Exercise, Kit };

struct Event {
    EventType type = EventType::Hit;
    Instrument instrument = Instrument::Unknown;
    uint8_t note = 0;
    uint8_t velocity = 0;    // as played (before the velocity curve)
    uint8_t controller = 0;
    uint8_t value = 0;
    bool playing = false;    // Hit: transport running at the hit frame (false -> graded Free)
    GarbageKind garbage = GarbageKind::None;
    double posTicks = 0;     // Hit: latency-compensated transport position; Stopped: end position
    double msPerTick = 0;
    int64_t frame = 0;       // engine frame time (frames processed since the engine started)
    void* ptr = nullptr;     // Garbage: object to delete on the app thread
};

// Optional schedule trace (tests, --trace): what the callback triggered and at which frame.
enum class TraceKind : uint8_t { Accent, Beat, Subdivision, Groove, Pad };
struct TraceEvent {
    TraceKind kind = TraceKind::Accent;
    Instrument instrument = Instrument::Unknown;
    uint8_t note = 0;  // kit note (Groove/Pad)
    uint8_t velocity = 0;
    int64_t frame = 0;
    double tick = 0;
};

struct TransportSnapshot {
    bool playing = false;
    double posTicks = 0;        // raw transport position at the end of the last block (not compensated)
    double bpm = 60;
    double msPerTick = 0;
    double compensationMs = 0;  // output latency + latency offset
    int64_t frame = 0;          // engine frame time at the end of the last block
    int64_t playStartFrame = 0;
};

class AudioEngine {
public:
    explicit AudioEngine(double sampleRate, bool trace = false);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    double sampleRate() const { return sampleRate_; }

    // Before audio starts only (not synchronized with process()).
    void installKit(std::unique_ptr<KitBundle> kit);
    void installExercise(std::unique_ptr<RtExercise> exercise);

    // App thread -> callback. False when the queue is full. Ownership of ptr payloads moves on success.
    bool send(const Command& command);
    // Callback -> app thread. Garbage events carry objects the app thread must destroy (see dispose()).
    bool poll(Event& event);
    static void dispose(const Event& garbage);
    bool pollTrace(TraceEvent& event);

    void setOutputLatencyFrames(int frames) { outputLatencyFrames_.store(frames, std::memory_order_relaxed); }
    int outputLatencyFrames() const { return outputLatencyFrames_.load(std::memory_order_relaxed); }
    TransportSnapshot snapshot() const;
    uint64_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

    // ---- real-time ----
    void process(int frames, const MidiEvent* events, int numEvents, float* outL, float* outR) noexcept;

private:
    void applyCommands() noexcept;
    void processChunk(int frames, const MidiEvent* events, int numEvents, int eventBase, float* outL,
                      float* outR) noexcept;
    void scheduleClicks(double p0, double p1, int64_t f0, int limit) noexcept;
    void scheduleGroove(double p0, double p1, int64_t f0, int limit) noexcept;
    void handleMidi(const MidiEvent& e, int offset, int64_t f0, int stopAt) noexcept;
    void startTransport() noexcept;
    void reanchor() noexcept;
    double endTickForLoopSetting() const noexcept;
    double ticksPerFrame() const noexcept;
    double msPerTick() const noexcept;
    double posAtFrame(int64_t frame) const noexcept;
    int64_t frameOfTick(double tick) const noexcept;
    int curveVelocity(Instrument i, int velocity) const noexcept;
    void pushEvent(const Event& e) noexcept;
    void pushGarbage(GarbageKind kind, void* ptr) noexcept;
    void trace(TraceKind kind, Instrument i, int note, int velocity, int64_t frame, double tick) noexcept;
    void publish() noexcept;

    const double sampleRate_;

    SpscQueue<Command> commands_{1024};
    SpscQueue<Event> events_{8192};
    std::unique_ptr<SpscQueue<TraceEvent>> trace_;
    std::array<Event, 16> pendingGarbage_{};
    int numPendingGarbage_ = 0;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<int> outputLatencyFrames_{0};

    // ---- state owned by the callback ----
    KitBundle* kit_ = nullptr;
    RtExercise* exercise_ = nullptr;
    int64_t frameTime_ = 0;

    bool playing_ = false;
    double bpm_ = 60.0;
    bool loop_ = true;
    int countInBars_ = 1;
    double startTick_ = 0;      // transport position at play (count-in start)
    double endTick_ = 0;        // transport stops here (infinity while looping)
    double anchorTick_ = 0;     // position at anchorFrame_ (re-anchored on tempo/exercise change)
    int64_t anchorFrame_ = 0;
    int64_t playStartFrame_ = 0;
    double lastPos_ = 0;

    bool clickOn_ = true;
    bool clickSubdivisions_ = true;
    bool grooveOn_ = false;
    double latencyOffsetMs_ = 0;

    // Mixer targets and the values reached at the end of the last block (for gain ramps).
    float master_ = 0.8f, masterNow_ = 0.8f;
    float clickVolume_ = 0.8f, clickNow_ = 0.8f;
    float grooveVolume_ = 0.7f;
    std::array<float, kNumInstruments> instrumentVolume_{};
    std::array<float, kNumOutputs> playerGains_{}, grooveGains_{};

    std::array<Instrument, 128> padMap_{};
    int hihatPedalCC_ = 4;
    std::array<float, kNumInstruments> velocityGain_{}, velocityExponent_{};

    ClickSynth click_;
    std::array<float, kChunkFrames> busL_{}, busR_{}, clickBus_{};

    // Seqlock-published snapshot.
    std::atomic<uint32_t> seq_{0};
    std::atomic<bool> snapPlaying_{false};
    std::atomic<double> snapPos_{0}, snapBpm_{60}, snapMsPerTick_{0}, snapCompMs_{0};
    std::atomic<int64_t> snapFrame_{0}, snapPlayStart_{0};

public:
    static constexpr double kNoEnd = std::numeric_limits<double>::infinity();
};

}  // namespace drums::engine
