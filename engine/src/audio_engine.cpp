#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace drums::engine {

// ---------------------------------------------------------------- commands

Command Command::click(std::optional<bool> on, std::optional<double> volume, std::optional<bool> subdivisions) {
    Command c{CommandType::SetClick};
    if (on) c.fields |= 1, c.on = *on;
    if (volume) c.fields |= 2, c.x = *volume;
    if (subdivisions) c.fields |= 4, c.flag = *subdivisions;
    return c;
}

Command Command::groove(std::optional<bool> on, std::optional<double> volume) {
    Command c{CommandType::SetGroove};
    if (on) c.fields |= 1, c.on = *on;
    if (volume) c.fields |= 2, c.x = *volume;
    return c;
}

Command Command::instrumentVolume(Instrument i, double v) {
    Command c{CommandType::SetInstrumentVolume};
    c.index = int32_t(i);
    c.x = v;
    return c;
}

Command Command::padNote(int note, Instrument i) {
    Command c{CommandType::SetPadNote};
    c.index = note;
    c.value = int32_t(i);
    return c;
}

Command Command::velocityCurve(Instrument i, double gain, double exponent) {
    Command c{CommandType::SetVelocityCurve};
    c.index = int32_t(i);
    c.x = gain;
    c.y = exponent;
    return c;
}

Command Command::exercise(std::unique_ptr<RtExercise> ex) {
    Command c{CommandType::SetExercise};
    c.ptr = ex.release();
    return c;
}

Command Command::kit(std::unique_ptr<KitBundle> kit) {
    Command c{CommandType::SetKit};
    c.ptr = kit.release();
    return c;
}

// ---------------------------------------------------------------- setup / app side

AudioEngine::AudioEngine(double sampleRate, bool trace) : sampleRate_(sampleRate) {
    if (trace) trace_ = std::make_unique<SpscQueue<TraceEvent>>(1 << 16);
    instrumentVolume_.fill(1.0f);
    velocityGain_.fill(1.0f);
    velocityExponent_.fill(1.0f);
    padMap_.fill(Instrument::Unknown);
    playerGains_.fill(1.0f);
    grooveGains_.fill(grooveVolume_);
    click_.prepare(sampleRate);
}

AudioEngine::~AudioEngine() {
    // The callback is stopped by now: free whatever is still in flight.
    Command c;
    while (commands_.pop(c)) {
        if (c.type == CommandType::SetExercise) delete static_cast<RtExercise*>(c.ptr);
        if (c.type == CommandType::SetKit) delete static_cast<KitBundle*>(c.ptr);
    }
    Event e;
    while (events_.pop(e)) dispose(e);
    for (int i = 0; i < numPendingGarbage_; ++i) dispose(pendingGarbage_[size_t(i)]);
    delete exercise_;
    delete kit_;
}

void AudioEngine::installKit(std::unique_ptr<KitBundle> kit) {
    delete kit_;
    kit_ = kit.release();
}

void AudioEngine::installExercise(std::unique_ptr<RtExercise> exercise) {
    delete exercise_;
    exercise_ = exercise.release();
}

bool AudioEngine::send(const Command& command) { return commands_.push(command); }

bool AudioEngine::poll(Event& event) { return events_.pop(event); }

void AudioEngine::dispose(const Event& e) {
    if (e.type != EventType::Garbage) return;
    if (e.garbage == GarbageKind::Exercise) delete static_cast<RtExercise*>(e.ptr);
    if (e.garbage == GarbageKind::Kit) delete static_cast<KitBundle*>(e.ptr);
}

bool AudioEngine::pollTrace(TraceEvent& event) { return trace_ && trace_->pop(event); }

TransportSnapshot AudioEngine::snapshot() const {
    TransportSnapshot s;
    for (;;) {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        s.playing = snapPlaying_.load(std::memory_order_relaxed);
        s.posTicks = snapPos_.load(std::memory_order_relaxed);
        s.bpm = snapBpm_.load(std::memory_order_relaxed);
        s.msPerTick = snapMsPerTick_.load(std::memory_order_relaxed);
        s.compensationMs = snapCompMs_.load(std::memory_order_relaxed);
        s.frame = snapFrame_.load(std::memory_order_relaxed);
        s.playStartFrame = snapPlayStart_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == s1) return s;
    }
}

// ---------------------------------------------------------------- real-time

double AudioEngine::msPerTick() const noexcept {
    const double beat = exercise_ ? double(exercise_->beatTicks) : 480.0;
    return 60000.0 / (bpm_ * beat);  // == Exercise::msPerTick(bpm)
}

double AudioEngine::ticksPerFrame() const noexcept { return 1000.0 / (msPerTick() * sampleRate_); }

double AudioEngine::posAtFrame(int64_t frame) const noexcept {
    return anchorTick_ + double(frame - anchorFrame_) * ticksPerFrame();
}

// First frame whose position is >= tick (the epsilon absorbs floating-point noise on exact grid points).
int64_t AudioEngine::frameOfTick(double tick) const noexcept {
    const double framesPerTick = msPerTick() * sampleRate_ / 1000.0;
    return anchorFrame_ + int64_t(std::ceil((tick - anchorTick_) * framesPerTick - 1e-6));
}

void AudioEngine::reanchor() noexcept {
    if (!playing_) return;
    anchorTick_ = posAtFrame(frameTime_);
    anchorFrame_ = frameTime_;
}

double AudioEngine::endTickForLoopSetting() const noexcept {
    if (loop_ || !exercise_) return kNoEnd;
    const double len = double(exercise_->lengthTicks);
    if (!playing_) return len;
    // Loop switched off while playing: finish the current pass.
    const double pos = posAtFrame(frameTime_);
    return pos < 0 ? len : (std::floor(pos / len) + 1.0) * len;
}

void AudioEngine::startTransport() noexcept {
    if (!exercise_) return;
    playing_ = true;
    startTick_ = -double(countInBars_) * double(exercise_->barTicks);
    anchorTick_ = startTick_;
    anchorFrame_ = frameTime_;
    playStartFrame_ = frameTime_;
    endTick_ = loop_ ? kNoEnd : double(exercise_->lengthTicks);
    lastPos_ = startTick_;
}

void AudioEngine::applyCommands() noexcept {
    // Retry garbage that did not fit in the event queue earlier.
    while (numPendingGarbage_ > 0 && events_.push(pendingGarbage_[size_t(numPendingGarbage_ - 1)]))
        --numPendingGarbage_;

    Command c;
    while (commands_.pop(c)) {
        switch (c.type) {
            case CommandType::Play:
                if (!playing_) startTransport();
                break;
            case CommandType::Stop:
                if (playing_) lastPos_ = posAtFrame(frameTime_);
                playing_ = false;
                break;
            case CommandType::SetBpm:
                reanchor();  // tempo changes take effect at this buffer boundary
                bpm_ = std::clamp(c.x, 20.0, 300.0);
                break;
            case CommandType::SetLoop:
                loop_ = c.on;
                endTick_ = endTickForLoopSetting();
                break;
            case CommandType::SetCountIn:
                countInBars_ = std::clamp(c.index, 0, 2);  // used at the next play
                break;
            case CommandType::SetClick:
                if (c.fields & 1) clickOn_ = c.on;
                if (c.fields & 2) clickVolume_ = float(std::clamp(c.x, 0.0, 1.0));
                if (c.fields & 4) clickSubdivisions_ = c.flag;
                break;
            case CommandType::SetGroove:
                if (c.fields & 1) grooveOn_ = c.on;
                if (c.fields & 2) grooveVolume_ = float(std::clamp(c.x, 0.0, 1.0));
                break;
            case CommandType::SetMasterVolume:
                master_ = float(std::clamp(c.x, 0.0, 1.5));
                break;
            case CommandType::SetInstrumentVolume:
                if (c.index >= 0 && size_t(c.index) < kNumInstruments)
                    instrumentVolume_[size_t(c.index)] = float(std::clamp(c.x, 0.0, 1.5));
                break;
            case CommandType::SetLatencyOffset:
                latencyOffsetMs_ = c.x;
                break;
            case CommandType::SetPadNote:
                if (c.index >= 0 && c.index < 128) padMap_[size_t(c.index)] = Instrument(c.value);
                break;
            case CommandType::SetHihatPedalCC:
                hihatPedalCC_ = c.index;
                break;
            case CommandType::SetVelocityCurve:
                if (c.index >= 0 && size_t(c.index) < kNumInstruments) {
                    velocityGain_[size_t(c.index)] = float(std::max(0.0, c.x));
                    velocityExponent_[size_t(c.index)] = float(std::clamp(c.y, 0.1, 10.0));
                }
                break;
            case CommandType::SetExercise: {
                reanchor();  // keep the position; the new beatTicks changes the tempo scale from here on
                RtExercise* old = exercise_;
                exercise_ = static_cast<RtExercise*>(c.ptr);
                if (!exercise_) playing_ = false;
                endTick_ = endTickForLoopSetting();
                pushGarbage(GarbageKind::Exercise, old);
                break;
            }
            case CommandType::SetKit: {
                KitBundle* old = kit_;
                kit_ = static_cast<KitBundle*>(c.ptr);
                pushGarbage(GarbageKind::Kit, old);
                break;
            }
        }
    }
}

void AudioEngine::process(int frames, const MidiEvent* events, int numEvents, float* outL, float* outR) noexcept {
    applyCommands();
    int e = 0;
    for (int done = 0; done < frames;) {
        const int len = std::min(frames - done, kChunkFrames);
        int end = e;
        while (end < numEvents && int(events[end].frame) < done + len) ++end;
        processChunk(len, events + e, end - e, done, outL + done, outR + done);
        e = end;
        done += len;
    }
    publish();
}

void AudioEngine::processChunk(int frames, const MidiEvent* events, int numEvents, int eventBase, float* outL,
                               float* outR) noexcept {
    const int64_t f0 = frameTime_;
    int stopAt = -1;  // frame (in this chunk) where the transport stops
    if (playing_ && exercise_) {
        const double p0 = posAtFrame(f0);
        const double p1 = posAtFrame(f0 + frames);
        if (endTick_ != kNoEnd) {
            const int64_t fe = frameOfTick(endTick_);
            if (fe < f0 + frames) stopAt = int(std::max<int64_t>(0, fe - f0));
        }
        const int limit = stopAt >= 0 ? stopAt : frames;
        if (clickOn_) scheduleClicks(p0, p1, f0, limit);
        if (grooveOn_ && kit_) scheduleGroove(p0, p1, f0, limit);
    }

    for (int i = 0; i < numEvents; ++i) handleMidi(events[i], int(events[i].frame) - eventBase, f0, stopAt);

    // ---- render + mix
    std::fill_n(busL_.begin(), frames, 0.0f);
    std::fill_n(busR_.begin(), frames, 0.0f);
    std::fill_n(clickBus_.begin(), frames, 0.0f);

    std::array<float, kNumOutputs> playerTarget, grooveTarget;
    playerTarget[0] = 1.0f;
    for (size_t i = 0; i < kNumInstruments; ++i) playerTarget[1 + i] = instrumentVolume_[i];
    for (size_t o = 0; o < size_t(kNumOutputs); ++o) grooveTarget[o] = playerTarget[o] * grooveVolume_;
    if (kit_) {
        kit_->player->renderAdd(frames, busL_.data(), busR_.data(), playerGains_.data(), playerTarget.data());
        kit_->groove->renderAdd(frames, busL_.data(), busR_.data(), grooveGains_.data(), grooveTarget.data());
    }
    playerGains_ = playerTarget;
    grooveGains_ = grooveTarget;

    click_.renderAdd(clickBus_.data(), frames);

    const float inv = 1.0f / float(frames);
    const float masterStep = (master_ - masterNow_) * inv;
    const float clickStep = (clickVolume_ - clickNow_) * inv;
    float m = masterNow_, cg = clickNow_;
    for (int f = 0; f < frames; ++f) {
        const float c = cg * clickBus_[size_t(f)];
        outL[f] = std::clamp(m * (busL_[size_t(f)] + c), -1.0f, 1.0f);
        outR[f] = std::clamp(m * (busR_[size_t(f)] + c), -1.0f, 1.0f);
        m += masterStep;
        cg += clickStep;
    }
    masterNow_ = master_;
    clickNow_ = clickVolume_;

    frameTime_ += frames;
    if (playing_) lastPos_ = posAtFrame(frameTime_);
    if (stopAt >= 0) {
        playing_ = false;
        lastPos_ = endTick_;
        Event ev;
        ev.type = EventType::Stopped;
        ev.posTicks = endTick_;
        ev.msPerTick = msPerTick();
        ev.frame = f0 + stopAt;
        pushEvent(ev);
    }
}

namespace {
int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }
}  // namespace

void AudioEngine::scheduleClicks(double p0, double p1, int64_t f0, int limit) noexcept {
    const int64_t bar = exercise_->barTicks, beat = exercise_->beatTicks, sub = exercise_->subdivisionTicks;
    const int64_t step = clickSubdivisions_ ? std::gcd(std::gcd(bar, beat), sub) : std::gcd(bar, beat);
    for (int64_t t = floorDiv(int64_t(std::floor(p0)), step) * step; double(t) < p1 + 1.0; t += step) {
        if (double(t) < startTick_) continue;
        if (double(t) >= endTick_) break;
        const int64_t k = frameOfTick(double(t)) - f0;
        if (k < 0) continue;
        if (k >= limit) break;
        const int64_t inBar = floorMod(t, bar);
        TraceKind kind;
        if (inBar == 0) kind = TraceKind::Accent;
        else if (inBar % beat == 0) kind = TraceKind::Beat;
        else if (clickSubdivisions_ && inBar % sub == 0) kind = TraceKind::Subdivision;
        else continue;
        click_.trigger(ClickKind(int(kind)), int(k));
        trace(kind, Instrument::Unknown, 0, 0, f0 + k, double(t));
    }
}

void AudioEngine::scheduleGroove(double p0, double p1, int64_t f0, int limit) noexcept {
    const int64_t len = exercise_->lengthTicks;
    const int64_t firstLoop = std::max<int64_t>(0, floorDiv(int64_t(std::floor(p0)), len));
    const int64_t lastLoop = floorDiv(int64_t(std::ceil(p1)), len);
    for (int64_t loop = firstLoop; loop <= lastLoop; ++loop) {
        for (const RtNote& n : exercise_->notes) {
            const int64_t t = loop * len + n.tick;
            if (double(t) >= endTick_) return;
            if (double(t) < startTick_) continue;
            const int64_t k = frameOfTick(double(t)) - f0;
            if (k < 0) continue;
            if (k >= limit) break;  // notes are sorted
            kit_->groove->noteOn(int(k), n.instrument, n.velocity);
            trace(TraceKind::Groove, n.instrument, kit_->groove->kitNote(n.instrument), n.velocity, f0 + k,
                  double(t));
        }
    }
}

int AudioEngine::curveVelocity(Instrument i, int velocity) const noexcept {
    const size_t idx = instrumentIndex(i);
    const double x = std::clamp(velocity, 0, 127) / 127.0;
    const double v = 127.0 * velocityGain_[idx] * std::pow(x, double(velocityExponent_[idx]));
    return std::clamp(int(std::lround(v)), 1, 127);
}

void AudioEngine::handleMidi(const MidiEvent& e, int offset, int64_t f0, int stopAt) noexcept {
    if (e.size == 0) return;
    const uint8_t status = e.data[0];
    // Running status is resolved by JACK; 0xF0-0xFF (sysex, MIDI clock 0xF8, active sensing...) ignored.
    if (status < 0x80 || status >= 0xF0) return;
    const uint8_t type = status & 0xF0;
    offset = std::clamp(offset, 0, kChunkFrames - 1);
    const int64_t frame = f0 + offset;

    if ((type == 0x90 || type == 0x80) && e.size >= 3) {
        const int note = e.data[1] & 0x7F;
        const int velocity = e.data[2] & 0x7F;
        const Instrument instrument = padMap_[size_t(note)];
        if (type == 0x90 && velocity > 0) {
            int kitNote = -1;
            const int played = instrument != Instrument::Unknown ? curveVelocity(instrument, velocity) : velocity;
            if (instrument != Instrument::Unknown && kit_) {
                kit_->player->noteOn(offset, instrument, played);
                kitNote = kit_->player->kitNote(instrument);
            }
            Event ev;
            ev.type = EventType::Hit;
            ev.instrument = instrument;
            ev.note = uint8_t(note);
            ev.velocity = uint8_t(velocity);
            ev.frame = frame;
            ev.msPerTick = msPerTick();
            ev.playing = playing_ && exercise_ && (stopAt < 0 || offset < stopAt);
            if (ev.playing) {
                const double compensationMs =
                    outputLatencyFrames() * 1000.0 / sampleRate_ + latencyOffsetMs_;
                ev.posTicks = posAtFrame(frame) - compensationMs / ev.msPerTick;
            }
            pushEvent(ev);
            trace(TraceKind::Pad, instrument, kitNote < 0 ? 0 : kitNote, played, frame,
                  ev.playing ? posAtFrame(frame) : 0.0);
        } else {
            if (instrument != Instrument::Unknown && kit_) kit_->player->noteOff(offset, instrument);
            Event ev;
            ev.type = EventType::NoteOff;
            ev.instrument = instrument;
            ev.note = uint8_t(note);
            ev.frame = frame;
            pushEvent(ev);
        }
    } else if (type == 0xB0 && e.size >= 3) {
        const int controller = e.data[1] & 0x7F;
        const int value = e.data[2] & 0x7F;
        if (controller == hihatPedalCC_ && kit_) kit_->player->hihatPedal(offset, value);
        Event ev;
        ev.type = EventType::Control;
        ev.controller = uint8_t(controller);
        ev.value = uint8_t(value);
        ev.frame = frame;
        pushEvent(ev);
    }
}

void AudioEngine::pushEvent(const Event& e) noexcept {
    if (!events_.push(e)) dropped_.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::pushGarbage(GarbageKind kind, void* ptr) noexcept {
    if (!ptr) return;
    Event e;
    e.type = EventType::Garbage;
    e.garbage = kind;
    e.ptr = ptr;
    if (events_.push(e)) return;
    if (numPendingGarbage_ < int(pendingGarbage_.size())) pendingGarbage_[size_t(numPendingGarbage_++)] = e;
    else dropped_.fetch_add(1, std::memory_order_relaxed);  // leaks; cannot happen with a drained queue
}

void AudioEngine::trace(TraceKind kind, Instrument i, int note, int velocity, int64_t frame, double tick) noexcept {
    if (!trace_) return;
    TraceEvent t;
    t.kind = kind;
    t.instrument = i;
    t.note = uint8_t(note);
    t.velocity = uint8_t(velocity);
    t.frame = frame;
    t.tick = tick;
    trace_->push(t);
}

void AudioEngine::publish() noexcept {
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    snapPlaying_.store(playing_, std::memory_order_relaxed);
    snapPos_.store(lastPos_, std::memory_order_relaxed);
    snapBpm_.store(bpm_, std::memory_order_relaxed);
    snapMsPerTick_.store(msPerTick(), std::memory_order_relaxed);
    snapCompMs_.store(outputLatencyFrames() * 1000.0 / sampleRate_ + latencyOffsetMs_, std::memory_order_relaxed);
    snapFrame_.store(frameTime_, std::memory_order_relaxed);
    snapPlayStart_.store(playStartFrame_, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
}

}  // namespace drums::engine
