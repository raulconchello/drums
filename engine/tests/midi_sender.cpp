// Test tool: a virtual ALSA sequencer MIDI source that behaves like the e-drum: a constant MIDI
// clock (0xF8) stream plus drum notes on channel 10.
//
//   midi-sender [--name e-drum-sim] [--delay S] [--count N] [--interval-ms MS] [--notes 36,38,42]
//               [--velocity V]
//
// Waits --delay seconds (so the port can be connected), then sends --count note-ons every
// --interval-ms (note-off 30 ms later), with MIDI clock every 20.833 ms throughout. Prints one line
// per note-on: "<monotonic ms since first note> <note> <velocity>".
#include <alsa/asoundlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    std::string name = "e-drum-sim";
    double delay = 2.0, intervalMs = 500.0;
    int count = 8, velocity = 100;
    std::vector<int> notes = {36, 38, 42};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--name") name = next();
        else if (a == "--delay") delay = std::atof(next());
        else if (a == "--count") count = std::atoi(next());
        else if (a == "--interval-ms") intervalMs = std::atof(next());
        else if (a == "--velocity") velocity = std::atoi(next());
        else if (a == "--notes") {
            notes.clear();
            std::string list = next();
            for (char* tok = std::strtok(list.data(), ","); tok; tok = std::strtok(nullptr, ","))
                notes.push_back(std::atoi(tok));
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        std::fprintf(stderr, "cannot open the ALSA sequencer\n");
        return 1;
    }
    snd_seq_set_client_name(seq, name.c_str());
    const std::string portName = name + " MIDI 1";
    const int port = snd_seq_create_simple_port(seq, portName.c_str(),
                                                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
        std::fprintf(stderr, "cannot create the port\n");
        return 1;
    }
    std::fprintf(stderr, "midi-sender: ALSA client %d '%s', port %d '%s'\n", snd_seq_client_id(seq), name.c_str(),
                 port, portName.c_str());

    auto send = [&](snd_seq_event_t& ev) {
        snd_seq_ev_set_source(&ev, port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);
    };
    auto clockTick = [&] {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        ev.type = SND_SEQ_EVENT_CLOCK;
        send(ev);
    };
    auto note = [&](int n, int v) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        if (v > 0) snd_seq_ev_set_noteon(&ev, 9, n, v);
        else snd_seq_ev_set_noteoff(&ev, 9, n, 0);
        send(ev);
    };

    // Event plan relative to t0 = end of the delay.
    struct Planned {
        double ms;
        int note;
        int velocity;  // 0 = note-off, -1 = clock
    };
    std::vector<Planned> plan;
    const double lastMs = (count - 1) * intervalMs + 500.0;
    for (double t = 0; t <= lastMs; t += 1000.0 / 48.0) plan.push_back({t, 0, -1});  // 24 ppqn @ 120 bpm
    for (int i = 0; i < count; ++i) {
        const double t = 100.0 + i * intervalMs;
        const int n = notes[size_t(i) % notes.size()];
        plan.push_back({t, n, velocity});
        plan.push_back({t + 30.0, n, 0});
    }
    std::stable_sort(plan.begin(), plan.end(), [](const Planned& a, const Planned& b) { return a.ms < b.ms; });

    // Keep the clock running during the delay too, like the real kit.
    const auto start = Clock::now();
    for (double t = 0; t < delay * 1000.0; t += 1000.0 / 48.0) {
        std::this_thread::sleep_until(start + std::chrono::microseconds(int64_t(t * 1000)));
        clockTick();
    }
    const auto t0 = start + std::chrono::microseconds(int64_t(delay * 1e6));
    int sentNotes = 0, sentClocks = 0;
    for (const auto& p : plan) {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(p.ms * 1000)));
        if (p.velocity < 0) {
            clockTick();
            ++sentClocks;
        } else {
            note(p.note, p.velocity);
            if (p.velocity > 0) {
                ++sentNotes;
                const double now = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                std::printf("%.3f %d %d\n", now, p.note, p.velocity);
                std::fflush(stdout);
            }
        }
    }
    std::fprintf(stderr, "midi-sender: sent %d note-ons and %d clock messages\n", sentNotes, sentClocks);
    snd_seq_close(seq);
    return 0;
}
