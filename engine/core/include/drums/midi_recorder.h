// Records pad input to a Standard MIDI File. CONTRACT FILE. Implemented in core/src. Not real-time.
#pragma once

#include "drums/types.h"

#include <memory>
#include <string>

namespace drums {

class MidiRecorder {
public:
    MidiRecorder();
    ~MidiRecorder();
    MidiRecorder(const MidiRecorder&) = delete;
    MidiRecorder& operator=(const MidiRecorder&) = delete;

    // quarterNoteBpm = bpm * beatTicks / ppq. Discards any previous unsaved take.
    void start(double quarterNoteBpm, TimeSignature timeSignature);
    bool isRecording() const;

    // timeMs: since start(). All events go to MIDI channel 10 (drums).
    void noteOn(int note, int velocity, double timeMs);
    void noteOff(int note, double timeMs);
    void controlChange(int controller, int value, double timeMs);

    // SMF format 0, 480 PPQ, tempo + time signature meta events. Creates parent dirs.
    // Returns false (and keeps the take) on IO error.
    bool stopAndSave(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace drums
