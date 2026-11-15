# Exercise file format

JSON. Two ways to write notes; a file may use either or both (they are merged and sorted).

```json
{
  "id": "groove-12-8-bar23",
  "title": "12/8 groove (bar 23)",
  "timeSignature": [12, 8],
  "ppq": 480,              // optional, default 480 (only 480 is supported for now)
  "beatTicks": 720,        // what bpm counts; default = one denominator note (480*4/den), 720 for 6/8, 9/8, 12/8
  "subdivisionTicks": 240, // metronome subdivision clicks; default = one denominator note
  "bars": 1,               // default 1; must cover every note
  "defaultBpm": 60,        // default 80

  "gridTicks": 240,        // length of one grid character
  "grid": {                // one string per instrument, one char per gridTicks step
    "hihatClosed": "xxx|xxx|xxx|xxx",
    "snare":       "...|.x.|x..|...",
    "kick":        "x..|..x|...|..x"
  },

  "notes": [               // explicit notes; tick counted from the start of the exercise
    { "instrument": "crash", "tick": 0, "velocity": 110 }
  ]
}
```

Grid characters: `x` = hit (velocity 100), `X` = accent (124), `g` = ghost (40), `.` or `-` = rest.
`|` and spaces are ignored (visual separators only). A grid row longer than `bars * barTicks / gridTicks`
steps is an error.

Instrument names: `kick snare snareRim hihatClosed hihatOpen hihatPedal tom1 tom2 tom3 crash ride rideBell`.
