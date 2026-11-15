# Engine ⇄ UI protocol (v1)

WebSocket, text frames, one JSON object per frame, every object has a `"type"`.
Engine listens on `ws://127.0.0.1:7878/` (override with `--port`).

## Units

- **ticks**: 480 per quarter note (`ppq`). A 12/8 bar is 12 × 240 = 2880 ticks.
- **bpm** counts the exercise's `beatTicks` (720 = dotted quarter in 12/8, 480 = quarter in 4/4).
- **posTicks**: continuous transport position since play. `0` = first tick of the exercise.
  Negative during the count-in. With loop on it keeps growing: `loop = floor(posTicks / lengthTicks)`,
  `tickInLoop = posTicks - loop * lengthTicks`.
- **errorMs**: hit time minus expected time, after latency compensation. Negative = early.
- Instrument names: `kick snare snareRim hihatClosed hihatOpen hihatPedal tom1 tom2 tom3 crash ride rideBell`.

## Engine → UI

| type | fields | when |
|---|---|---|
| `hello` | `protocol` (1), `sampleRate`, `bufferFrames`, `outputLatencyMs` | on connect |
| `state` | see below | on connect and after every change |
| `position` | `playing`, `posTicks`, `loop`, `tickInLoop`, `timeMs` | ~60 Hz while playing, once on stop |
| `hit` | `instrument`, `note`, `velocity`, `loop`, `tickInLoop`, `expectedTick` (int or null), `errorMs` (number or null), `grade` | every pad hit |
| `missed` | `instrument`, `loop`, `expectedTick` | an expected note's window closed with no hit |
| `loopSummary` | `loop`, `instruments: {name: stats}`, `total: stats` | after each loop's last window closes |
| `recordingSaved` | `path` | after `stopRecording` |
| `error` | `message` | bad command, failed load, … |

`grade`: `onTime` (|errorMs| ≤ 15), `early`, `late`, `extra` (no expected note matched; `expectedTick`/`errorMs` null),
`free` (transport stopped; `loop`/`tickInLoop` null).

`stats` = `{ "hits": int, "missed": int, "extra": int, "meanErrorMs": number, "stdDevMs": number }`.

`state`:
```json
{
  "type": "state",
  "playing": false, "recording": false,
  "bpm": 60, "loop": true, "countInBars": 1,
  "click":  { "on": true,  "volume": 0.8, "subdivisions": true },
  "groove": { "on": false, "volume": 0.7 },
  "latencyOffsetMs": 0,
  "exercise": { "id": "...", "title": "...", "timeSignature": [12, 8], "ppq": 480, "beatTicks": 720,
                "subdivisionTicks": 240, "bars": 1, "defaultBpm": 60,
                "notes": [ { "instrument": "kick", "tick": 0, "velocity": 100 } ] },
  "kit":  { "name": "...", "path": "..." },
  "kits": [ { "name": "...", "path": "..." } ],
  "mixer": { "master": 0.8, "kick": 1.0, "snare": 1.0 },
  "padMap": { "36": "kick", "38": "snare" }
}
```
`exercise` is always the canonical form (a `notes` array, sorted by tick), never grid strings.

## UI → Engine

| type | fields |
|---|---|
| `play` / `stop` | — |
| `setBpm` | `bpm` (20–300) |
| `setLoop` | `on` |
| `setCountIn` | `bars` (0–2) |
| `setClick` | any of `on`, `volume` (0–1), `subdivisions` |
| `setGroove` | any of `on`, `volume` (0–1) |
| `loadExercise` | `path` (relative to the repo root or absolute) |
| `loadKit` | `path` (a kit folder's `kit.json`) |
| `setMixer` | `channel` (instrument name or `master`), `volume` (0–1.5) |
| `setLatencyOffset` | `ms` (−50 – 100) |
| `startRecording` / `stopRecording` | — |
| `setPadMap` | `note` (0–127), `instrument` (name, or `null` to unmap) |
| `getState` | — |

Unknown or malformed commands get an `error` reply; the connection stays open.
