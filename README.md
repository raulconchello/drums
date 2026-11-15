# Drums

Practice app for an electronic drum kit on Linux. Plug the e-drum in over USB MIDI and your headphones into the PC.
The engine plays the drum sounds for your hits, a metronome and the exercise's groove. The UI shows the bar in
drum notation, where each hit landed (early, late, missed), and per-loop timing stats.

See `docs/architecture.html` for the overall design, `docs/PROTOCOL.md` for the engine ⇄ UI messages and
`docs/EXERCISE_FORMAT.md` for writing exercises.

## Run

```sh
./install.sh     # downloads the kits, builds engine + UI, adds "Drums" to the app launcher and a `drums` command
./uninstall.sh   # removes the launcher entry and the command (keeps the project, kits and recordings)
```

Re-run `./install.sh` after changing the code. Opening Drums starts `engine/build/drums-engine` (unless something
already listens on port 7878) and closing the window stops it. The engine connects to the e-drum (any MIDI port with "e-drum" in its name) and to the default
headphone/speaker output.

First time with the real kit:
- **Pad learn**: click a pad in the pad strip, then hit that drum, to map its MIDI note.
- **Latency offset**: if every hit is graded late by about the same amount, raise the offset by that much.

## Develop

| What | Command |
|---|---|
| UI with the fake engine | `cd ui && npm run mock` and, in another terminal, `npm run dev` |
| UI tests | `cd ui && npm run typecheck && npm test` |
| Engine tests | `cd engine/build && mise exec -- ctest` |
| Core library tests | `mise exec -- cmake -S engine/core -B engine/core/build -G Ninja && mise exec -- cmake --build engine/core/build && (cd engine/core/build && mise exec -- ctest)` |
| Engine options | `engine/build/drums-engine --help` |

## Layout

- `engine/src/`: real-time audio engine (PipeWire via the JACK API, sfizz sampler, transport, metronome, groove, mixer).
- `engine/core/`: scoring, exercises, MIDI recording and the WebSocket server (`drums_core` library).
- `ui/`: Electron + TypeScript + VexFlow UI, plus `ui/mock/` (fake engine for development).
- `exercises/`: exercise files. `kits/`: drum kits (downloaded) and the e-drum pad map.
- `recordings/`: your recorded takes (`.mid`).

Drum kits: AVL Black Pearl and AVL Red Zeppelin (CC-BY-SA 3.0) and Sam's Sonor (CC-BY-SA 4.0); see `kits/README.md`.
